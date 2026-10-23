#pragma once

#include "quark/Attributes.h"
#include "quark/QuarkError.h"

#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <utility>

QUARK_BEGIN_NAMESPACE

namespace net {

/// Linear receive buffer: recv() into write_ptr()/write_size(), then append(n);
/// parse from read_ptr()/read_size(), then consume(n). Unconsumed bytes are
/// moved to the front of the buffer when free space at the tail runs low.
class RxBuf {
public:
    // cache-line aligned so SIMD parsers can read the buffer efficiently
    static constexpr int kAlignment = 64;

    explicit RxBuf(int capacity) {
        assert(capacity > 0);
        // aligned_alloc requires the size to be a multiple of the alignment
        buf_size_ = (capacity + kAlignment - 1) & ~(kAlignment - 1);
        buf_ = static_cast<char *>(std::aligned_alloc(kAlignment, buf_size_));
        if (!buf_) {
            QUARK_THROW(QuarkError{"RxBuf allocation failed"});
        }
    }

    ~RxBuf() { std::free(buf_); }

    RxBuf(const RxBuf&) = delete;
    RxBuf& operator=(const RxBuf&) = delete;

    RxBuf(RxBuf&& rhs) noexcept
        : buf_(std::exchange(rhs.buf_, nullptr)),
          buf_size_(std::exchange(rhs.buf_size_, 0)),
          read_pos_(std::exchange(rhs.read_pos_, 0)),
          write_pos_(std::exchange(rhs.write_pos_, 0)) {}

    RxBuf& operator=(RxBuf&& rhs) noexcept {
        std::swap(buf_, rhs.buf_);
        std::swap(buf_size_, rhs.buf_size_);
        std::swap(read_pos_, rhs.read_pos_);
        std::swap(write_pos_, rhs.write_pos_);
        return *this;
    }

    int capacity() const noexcept { return buf_size_; }

    void clear() noexcept {
        read_pos_ = 0;
        write_pos_ = 0;
    }

    char* read_ptr() noexcept { return buf_ + read_pos_; }
    const char* read_ptr() const noexcept { return buf_ + read_pos_; }

    int read_size() const noexcept { return write_pos_ - read_pos_; }

    char* write_ptr() noexcept { return buf_ + write_pos_; }

    int write_size() const noexcept { return buf_size_ - write_pos_; }

    void consume(int size) noexcept {
        assert(size >= 0 && size <= read_size());
        read_pos_ += size;
        if (read_pos_ == write_pos_) {
            // everything parsed: rewind for free, no copy needed
            clear();
        } else if (read_pos_ > 0 &&
                   (read_pos_ >= buf_size_ / 2 || write_size() < low_watermark())) {
            compact();
        }
    }

    void append(int size) noexcept {
        assert(size >= 0 && size <= write_size());
        write_pos_ += size;
    }

private:
    char* buf_;
    int buf_size_;
    int read_pos_{0};
    int write_pos_{0};

    // compact once free tail space drops below 1/8 of the buffer
    int low_watermark() const noexcept { return buf_size_ / 8; }

    void compact() noexcept {
        const int n = read_size();
        std::memmove(buf_, buf_ + read_pos_, n);
        read_pos_ = 0;
        write_pos_ = n;
    }
};

} // namespace net

QUARK_END_NAMESPACE
