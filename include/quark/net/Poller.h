#pragma once

#include "quark/Attributes.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <limits>

QUARK_BEGIN_NAMESPACE

namespace net {

enum ReadyFlags : uint32_t {
    kReadable = 1u << 0,
    kWritable = 1u << 1,
    kError = 1u << 2,  ///< EPOLLERR / EPOLLHUP
};

struct ReadyEvent {
    void* token;     ///< the value passed to add()
    uint32_t flags;  ///< OR of ReadyFlags
};

/// Edge-triggered readiness notification (epoll). Users must drain a socket (read until
/// -EAGAIN) on every edge, or data already received never triggers another one.
class Poller {
public:
    static constexpr int kMaxBatch = 64;

    Poller() : handle_{::epoll_create1(EPOLL_CLOEXEC)} {}
    ~Poller() {
        if (handle_ >= 0) ::close(handle_);
    }

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    [[nodiscard]] bool valid() const noexcept { return handle_ >= 0; }

    bool add(int fd, void* token, bool want_write) noexcept { return ctl(EPOLL_CTL_ADD, fd, token, want_write); }
    bool modify(int fd, void* token, bool want_write) noexcept { return ctl(EPOLL_CTL_MOD, fd, token, want_write); }
    bool remove(int fd) noexcept { return ::epoll_ctl(handle_, EPOLL_CTL_DEL, fd, nullptr) == 0; }

    /// Waits up to `timeout_ns` (0: don't wait, < 0: forever) and fills up to `max` events.
    /// Returns the number of events, 0 on timeout or EINTR, -1 on error.
    int wait(ReadyEvent* out, int max, int64_t timeout_ns) noexcept {
        int timeout_ms = 0;
        if (timeout_ns < 0) {
            timeout_ms = -1;
        } else if (timeout_ns > 0) {
            const int64_t ms = (timeout_ns + 999'999) / 1'000'000;
            timeout_ms = ms > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(ms);
        }
        epoll_event evs[kMaxBatch];
        const int n = ::epoll_wait(handle_, evs, max < kMaxBatch ? max : kMaxBatch, timeout_ms);
        if (n < 0) return errno == EINTR ? 0 : -1;
        for (int i = 0; i < n; ++i) {
            uint32_t f = 0;
            if (evs[i].events & EPOLLIN) f |= kReadable;
            if (evs[i].events & EPOLLOUT) f |= kWritable;
            if (evs[i].events & (EPOLLERR | EPOLLHUP)) f |= kError;
            out[i] = ReadyEvent{evs[i].data.ptr, f};
        }
        return n;
    }

private:
    int handle_;

    bool ctl(int op, int fd, void* token, bool want_write) noexcept {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET | (want_write ? static_cast<uint32_t>(EPOLLOUT) : 0u);
        ev.data.ptr = token;
        return ::epoll_ctl(handle_, op, fd, &ev) == 0;
    }
};

} // namespace net

QUARK_END_NAMESPACE
