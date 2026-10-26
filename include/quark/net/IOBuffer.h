#pragma once

#include "quark/Attributes.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

QUARK_BEGIN_NAMESPACE

namespace net {

class BufferPool;

/// Non-owning handle to one fixed-size buffer of a BufferPool:
///
///     [ headroom | data (size) | tailroom ]
///
/// Acquiring with reserved headroom lets a protocol layer prepend() its header in front of a
/// payload that is already in place, so the payload is never moved on send.
///
/// Move-only, so exactly one handle owns a buffer and it can't be released twice through a copy.
/// The size arguments of append/prepend/consume/reset are preconditions (asserted in debug).
class IOBuffer {
public:
    IOBuffer() = default;
    IOBuffer(const IOBuffer&) = delete;
    IOBuffer& operator=(const IOBuffer&) = delete;
    IOBuffer(IOBuffer&& o) noexcept : base_{o.base_}, cap_{o.cap_}, head_{o.head_}, len_{o.len_} { o.clear(); }
    IOBuffer& operator=(IOBuffer&& o) noexcept {
        if (this != &o) {
            assert(!valid() && "IOBuffer: overwriting a buffer that was not released");
            base_ = o.base_;
            cap_ = o.cap_;
            head_ = o.head_;
            len_ = o.len_;
            o.clear();
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept { return base_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] uint8_t* data() noexcept { return base_ + head_; }
    [[nodiscard]] const uint8_t* data() const noexcept { return base_ + head_; }
    [[nodiscard]] std::size_t size() const noexcept { return len_; }
    [[nodiscard]] bool empty() const noexcept { return len_ == 0; }
    [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }
    [[nodiscard]] std::size_t headroom() const noexcept { return head_; }
    [[nodiscard]] std::size_t tailroom() const noexcept { return cap_ - head_ - len_; }

    [[nodiscard]] QUARK_ALWAYS_INLINE std::span<uint8_t> bytes() noexcept { return {data(), len_}; }
    [[nodiscard]] std::span<const uint8_t> bytes() const noexcept { return {data(), len_}; }

    /// Writable region after the data, e.g. to recv() into before append().
    [[nodiscard]] QUARK_ALWAYS_INLINE std::span<uint8_t> spare() noexcept { return {data() + len_, tailroom()}; }

    /// Grows the data by `n` bytes at the back and returns them for the caller to fill.
    QUARK_ALWAYS_INLINE std::span<uint8_t> append(std::size_t n) noexcept {
        assert(n <= tailroom());
        uint8_t* p = data() + len_;
        len_ += n;
        return {p, n};
    }

    /// Grows the data by `n` bytes at the front, taken from the headroom, and returns them.
    QUARK_ALWAYS_INLINE std::span<uint8_t> prepend(std::size_t n) noexcept {
        assert(n <= head_);
        head_ -= n;
        len_ += n;
        return {base_ + head_, n};
    }

    /// Drops `n` bytes from the front of the data.
    QUARK_ALWAYS_INLINE void consume(std::size_t n) noexcept {
        assert(n <= len_);
        head_ += n;
        len_ -= n;
    }

    /// Empties the buffer, keeping `reserve_front` bytes of headroom.
    QUARK_ALWAYS_INLINE void reset(std::size_t reserve_front = 0) noexcept {
        assert(reserve_front <= cap_);
        head_ = reserve_front;
        len_ = 0;
    }

    /// Moves the data to the front of the buffer (no headroom left) to maximise tailroom.
    void compact() noexcept {
        if (head_ == 0) return;
        if (len_ > 0) std::memmove(base_, base_ + head_, len_);
        head_ = 0;
    }

private:
    friend class BufferPool;

    void clear() noexcept {
        base_ = nullptr;
        cap_ = head_ = len_ = 0;
    }

    uint8_t* base_{nullptr};
    std::size_t cap_{0};
    std::size_t head_{0};
    std::size_t len_{0};
};

} // namespace net

QUARK_END_NAMESPACE
