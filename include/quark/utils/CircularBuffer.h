#pragma once

#include "quark/Attributes.h"

#include <algorithm
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>

QUARK_BEGIN_NAMESPACE

/// A makeshift queue.
/// Uses a circular buffer under the hood, so effectively zero-malloc (after
/// the initial allocation), provided elements are dequeued with a decent pace
/// to keep number of elements in the queue small.
template <class T>
    requires(std::is_nothrow_destructible_v<T>)
class CircularBuffer {
    T* buffer_;
    size_t capacity_;
    T* head_;
    T* tail_;
    size_t size_;

  public:
    explicit CircularBuffer(size_t capacity)
      : buffer_(static_cast<T*>(std::malloc(capacity * sizeof(T)))),
        capacity_(capacity),
        head_(buffer_),
        tail_(buffer_),
        size_(0) {}

    CircularBuffer(CircularBuffer<T>&& rhs) noexcept
      : buffer_(std::exchange(rhs.buffer_, nullptr)),
        capacity_(std::exchange(rhs.capacity_, 0)),
        head_(std::exchange(rhs.head_, nullptr)),
        tail_(std::exchange(rhs.tail_, nullptr)),
        size_(std::exchange(rhs.size_, 0)) {}

    CircularBuffer& operator=(CircularBuffer<T> rhs) noexcept {
        std::swap(buffer_, rhs.buffer_);
        std::swap(capacity_, rhs.capacity_);
        std::swap(head_, rhs.head_);
        std::swap(tail_, rhs.tail_);
        std::swap(size_, rhs.size_);
        return *this;
    }

    ~CircularBuffer() {
        auto destroy_range = [](std::span<T> range) {
            std::destroy(range.begin(), range.end());
        };
        destroy_range(first_range());
        destroy_range(second_range());
        std::free(buffer_);
    }

    size_t capacity() const noexcept { return capacity_; }
    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    T& front() { return *head_; }
    void pop_front() {
        head_->~T();
        if (++head_ == buffer_ + capacity_) {
            head_ = buffer_;
        }
        --size_;
    }

    template <class U> void push_back(U&& u) {
        if (size_ == capacity_) {
            grow();
        }
        new (tail_) T(std::forward<U>(u));
        if (++tail_ == buffer_ + capacity_) {
            tail_ = buffer_;
        }
        ++size_;
    }

    template <class... Args> void emplace_back(Args&&... args) {
        if (size_ == capacity_) {
            grow();
        }
        new (tail_) T(std::forward<Args>(args)...);
        if (++tail_ == buffer_ + capacity_) {
            tail_ = buffer_;
        }
        ++size_;
    }

    template <std::invocable<T&> Fn> void for_each_item(Fn&& fn) {
        for (T& elem : first_range()) {
            std::forward<Fn>(fn)(elem);
        }
        for (T& elem : second_range()) {
            std::forward<Fn>(fn)(elem);
        }
    }

    template <std::forward_iterator It>
        requires(std::constructible_from<T, std::iter_reference_t<It>>)
    void append(It begin, It end) {
        size_t sz = std::distance(begin, end);
        if (size_ + sz > capacity_) {
            grow(size_ + sz);
        }

        if (tail_ + sz <= buffer_ + capacity_) {
            // Free space unfragmented
            tail_ = move_range(begin, end, tail_);
        } else {
            auto bound = begin;
            std::advance(bound, std::distance(tail_, buffer_ + capacity_));
            move_range(begin, bound, tail_);
            tail_ = move_range(bound, end, buffer_);
        }
        size_ += sz;
        if (tail_ == buffer_ + capacity_) {
            tail_ = buffer_;
        }
    }

    template <std::output_iterator<T> It>
    std::pair<It, size_t /*elem count*/> pop_front_to(size_t n, It dst) {
        assert(n <= size_);
        size_t n1 = std::min<size_t>(
                n, static_cast<size_t>(buffer_ + capacity_ - head_));
        dst = std::move(head_, head_ + n1, dst);
        std::destroy(head_, head_ + n1);
        head_ += n1;
        if (head_ == buffer_ + capacity_) {
            head_ = buffer_;
        }
        size_ -= n1;

        n = std::min<size_t>(n - n1, static_cast<size_t>(tail_ - buffer_));
        if (n) {
            dst = std::move(buffer_, buffer_ + n, dst);
            std::destroy(buffer_, buffer_ + n);
            head_ = buffer_ + n;
            size_ -= n;
        }

        return {dst, n + n1};
    }

  private:
    std::span<T> first_range() noexcept {
        if (empty()) {
            return {};
        } else if (head_ < tail_) {
            return {head_, tail_};
        } else {
            return {head_, buffer_ + capacity_};
        }
    }

    std::span<const T> first_range() const noexcept {
        if (empty()) {
            return {};
        } else if (head_ < tail_) {
            return {head_, tail_};
        } else {
            return {head_, buffer_ + capacity_};
        }
    }

    std::span<T> second_range() noexcept {
        if (empty() || head_ < tail_) {
            return {};
        } else {
            return {buffer_, tail_};
        }
    }

    std::span<const T> second_range() const noexcept {
        if (empty() || head_ < tail_) {
            return {};
        } else {
            return {buffer_, tail_};
        }
    }

    void grow(size_t newCapacity = 0) {
        CircularBuffer q(std::max<size_t>(std::max<size_t>(newCapacity, 8),
                                 capacity_ * 2));
        auto dst = move_range(first_range(), q.tail_);
        q.tail_ = move_range(second_range(), dst);
        q.size_ = size_;
        *this = std::move(q);
    }

    static auto move_range(auto it, auto ie, auto dst) {
        if constexpr (std::is_nothrow_move_constructible_v<T> ||
                      !std::is_copy_constructible_v<T>) {
            return std::uninitialized_move(it, ie, dst);
        } else {
            return std::uninitialized_copy(it, ie, dst);
        }
    }

    static auto move_range(auto&& range, auto dst) {
        return move_range(range.begin(), range.end(), dst);
    }
};

QUARK_END_NAMESPACE
