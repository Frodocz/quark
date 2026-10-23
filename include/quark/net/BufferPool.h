#pragma once

#include "quark/Attributes.h"
#include "quark/QuarkError.h"
#include "quark/Sanitizers.h"
#include "quark/net/IOBuffer.h"
#include "quark/utils/MemoryArena.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

QUARK_BEGIN_NAMESPACE

namespace net {

/// Fixed pool of equal-sized I/O buffers carved out of a MemoryArena, handed out as IOBuffer handles.
///
/// - Single-threaded: one pool per event loop; acquire/release must run on the loop thread.
/// - The memory comes from a MemoryArena (huge pages when large enough, mlock'ed, pre-faulted),
///   either owned by the pool or shared with other pools of the same loop.
/// - Buffers are cache-line sized and aligned (buffer_size() rounds up to 64 bytes).
/// - Free buffers are kept on a LIFO index stack: release() never touches buffer memory, and the
///   most recently released (cache-hot) buffer is reused first.
/// - Debug builds catch double release and foreign handles; ASan builds poison free buffers.
class BufferPool {
public:
    static constexpr std::size_t kAlignment = 64;

    /// Pool with its own arena.
    /// @param count     number of buffers
    /// @param buf_size  bytes per buffer (headroom + data), rounded up to a multiple of 64
    BufferPool(std::size_t count, std::size_t buf_size, const MemoryArena::Options& options = {})
        : buf_size_{checked_buffer_size(count, buf_size)}, count_{count} {
        owned_arena_.emplace(count_ * buf_size_, options);
        init(*owned_arena_);
    }

    /// Pool carved out of `arena`, which must outlive it. Throws QuarkError if the arena doesn't
    /// have count * buffer_size() bytes left.
    BufferPool(MemoryArena& arena, std::size_t count, std::size_t buf_size)
        : buf_size_{checked_buffer_size(count, buf_size)}, count_{count} {
        init(arena);
    }

    ~BufferPool() = default;

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    /// Returns an empty buffer with `reserve_front` bytes of headroom, or an invalid handle
    /// (operator bool() == false) when the pool is exhausted.
    [[nodiscard]] IOBuffer acquire(std::size_t reserve_front = 0) noexcept {
        assert(reserve_front <= buf_size_);
        if (free_.empty()) [[unlikely]] return {};
        const uint32_t idx = free_.back();
        free_.pop_back();
#ifndef NDEBUG
        in_use_[idx] = 1;
#endif
        IOBuffer buf;
        buf.base_ = base_ + std::size_t{idx} * buf_size_;
        QUARK_ASAN_UNPOISON(buf.base_, buf_size_);
        buf.cap_ = buf_size_;
        buf.head_ = reserve_front;
        buf.len_ = 0;
        return buf;
    }

    /// Returns `buf` to the pool and clears the handle. No-op for an invalid handle.
    void release(IOBuffer& buf) noexcept {
        if (!buf) return;
        const std::size_t off = static_cast<std::size_t>(buf.base_ - base_);
        assert(buf.base_ >= base_ && off < count_ * buf_size_ && off % buf_size_ == 0 &&
               "BufferPool::release(): buffer not from this pool");
        const auto idx = static_cast<uint32_t>(off / buf_size_);
#ifndef NDEBUG
        assert(in_use_[idx] && "BufferPool::release(): double release");
        in_use_[idx] = 0;
#endif
        QUARK_ASAN_POISON(buf.base_, buf_size_);
        free_.push_back(idx);  // never reallocates: reserved to count_
        buf = IOBuffer{};
    }

    [[nodiscard]] std::size_t available() const noexcept { return free_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return count_; }
    [[nodiscard]] std::size_t buffer_size() const noexcept { return buf_size_; }
    /// Pages backing the buffers.
    [[nodiscard]] MemoryArena::PageKind page_kind() const noexcept { return page_kind_; }

private:
    std::optional<MemoryArena> owned_arena_;
    uint8_t* base_{nullptr};
    const std::size_t buf_size_;
    const std::size_t count_;
    MemoryArena::PageKind page_kind_{MemoryArena::PageKind::Normal};
    std::vector<uint32_t> free_;
#ifndef NDEBUG
    std::vector<uint8_t> in_use_;
#endif

    static std::size_t checked_buffer_size(std::size_t count, std::size_t buf_size) {
        const std::size_t rounded = (buf_size + kAlignment - 1) & ~(kAlignment - 1);
        if (count == 0 || count > std::numeric_limits<uint32_t>::max() || buf_size == 0 || rounded < buf_size ||
            rounded > std::numeric_limits<std::size_t>::max() / count) {
            QUARK_THROW(QuarkError{"BufferPool: invalid buffer count or size"});
        }
        return rounded;
    }

    void init(MemoryArena& arena) {
        base_ = static_cast<uint8_t*>(arena.allocate(count_ * buf_size_, kAlignment));
        if (base_ == nullptr) {
            QUARK_THROW(QuarkError{"BufferPool: not enough space left in the arena"});
        }
        page_kind_ = arena.page_kind();
        free_.reserve(count_);
        // pushed back to front so buffers are handed out in address order
        for (std::size_t i = count_; i-- > 0;) free_.push_back(static_cast<uint32_t>(i));
#ifndef NDEBUG
        in_use_.assign(count_, 0);
#endif
        QUARK_ASAN_POISON(base_, count_ * buf_size_);
    }
};

} // namespace net

QUARK_END_NAMESPACE
