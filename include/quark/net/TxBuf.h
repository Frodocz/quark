#pragma once

#include "quark/Attributes.h"
#include "quark/QuarkError.h"
#include "quark/math/MathOps.h"

#include <sys/uio.h>

#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <new>
#include <utility>

QUARK_BEGIN_NAMESPACE

namespace net {

/// Power-of-2 ring buffer for outgoing bytes. read_pos_ and write_pos_ are
/// free-running counters that are only masked when indexing, so the full
/// capacity is usable and size() == write_pos_ - read_pos_ (mod 2^32).
class TxBuf {
public:
    explicit TxBuf(int capacity) {
        assert(capacity > 0 && capacity <= (1 << 30));
        buf_size_ = std::bit_ceil(static_cast<uint32_t>(capacity));
        buf_ = static_cast<char *>(std::malloc(buf_size_));
        if (!buf_) {
            QUARK_THROW(QuarkError{"TxBuf allocation failed"});
        }
    }

    ~TxBuf() { std::free(buf_); }

    TxBuf(const TxBuf &) = delete;
    TxBuf &operator=(const TxBuf &) = delete;

    TxBuf(TxBuf&& rhs) noexcept
        : buf_(std::exchange(rhs.buf_, nullptr)),
          buf_size_(std::exchange(rhs.buf_size_, 0)),
          write_pos_(std::exchange(rhs.write_pos_, 0)),
          read_pos_(std::exchange(rhs.read_pos_, 0)) {}

    TxBuf& operator=(TxBuf&& rhs) noexcept {
        std::swap(buf_, rhs.buf_);
        std::swap(buf_size_, rhs.buf_size_);
        std::swap(write_pos_, rhs.write_pos_);
        std::swap(read_pos_, rhs.read_pos_);
        return *this;
    }

    int capacity() const noexcept { return static_cast<int>(buf_size_); }

    void clear() noexcept {
        write_pos_ = 0;
        read_pos_ = 0;
    }

    int size() const noexcept { return static_cast<int>(write_pos_ - read_pos_); }

    int free_size() const noexcept { return capacity() - size(); }

    bool append(const char* data, int data_size) noexcept {
        assert(data_size >= 0);
        if (data_size > free_size()) return false;
        if (data_size == 0) return true;
        const uint32_t w = write_pos_ & pos_mask();
        const uint32_t n = static_cast<uint32_t>(data_size);
        const uint32_t first = quark::math::min(n, buf_size_ - w);
        std::memcpy(buf_ + w, data, first);
        if (n > first) {
            std::memcpy(buf_, data + first, n - first);
        }
        write_pos_ += n;
        return true;
    }

    /// Zero-copy write: returns a pointer to at least `n` contiguous writable
    /// bytes, or nullptr if they are not available without wrapping (fall back
    /// to append() in that case). Follow with commit(m), m <= n, before any
    /// other write to the buffer.
    char* reserve(int n) noexcept {
        assert(n >= 0);
        if (n > contiguous_free_size()) return nullptr;
        return buf_ + (write_pos_ & pos_mask());
    }

    void commit(int n) noexcept {
        assert(n >= 0 && n <= contiguous_free_size());
        write_pos_ += static_cast<uint32_t>(n);
    }

    void consume(int data_size) noexcept {
        assert(data_size >= 0);
        data_size = quark::math::min(size(), data_size);
        read_pos_ += static_cast<uint32_t>(data_size);
        if (read_pos_ == write_pos_) {
            // empty: rewind so the next writes and reserve() get the longest contiguous run
            clear();
        }
    }

    /// Fills `iov` with the pending bytes, ready for writev()/sendmsg().
    /// Returns the number of iovec entries used (0, 1 or 2).
    int get_out_data(struct iovec (&iov)[2]) const noexcept {
        const uint32_t len = write_pos_ - read_pos_;
        if (len == 0) return 0;
        const uint32_t r = read_pos_ & pos_mask();
        const uint32_t first = quark::math::min(len, buf_size_ - r);
        iov[0].iov_base = buf_ + r;
        iov[0].iov_len = first;
        if (first == len) return 1;
        iov[1].iov_base = buf_;
        iov[1].iov_len = len - first;
        return 2;
    }

private:
    // ring buffer
    char *buf_;
    uint32_t buf_size_;
    uint32_t write_pos_ = 0;
    uint32_t read_pos_ = 0;

    uint32_t pos_mask() const noexcept { return buf_size_ - 1; }

    int contiguous_free_size() const noexcept {
        const uint32_t w = write_pos_ & pos_mask();
        return quark::math::min(free_size(), static_cast<int>(buf_size_ - w));
    }
};

} // namespace net

QUARK_END_NAMESPACE
