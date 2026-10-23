#pragma once

#include "quark/Attributes.h"

#include <cstdint>

QUARK_BEGIN_NAMESPACE

namespace net {

/// Detects a stale connection from traffic timestamps (ns, any monotonic clock). Two checks,
/// each disabled when its limit is 0:
///
/// - recv gap: nothing received for longer than `max_recv_gap_ns` (streams that should always
///   be ticking, e.g. market data).
/// - unacked request: a request sent while nothing has been received since stays unanswered
///   for longer than `max_ack_delay_ns` (request/response links, e.g. order entry, where a quiet
///   line is normal but an unanswered request is not). Any receive counts as the answer.
class AliveMonitor {
public:
    AliveMonitor(int64_t max_recv_gap_ns, int64_t max_ack_delay_ns) noexcept
        : max_recv_gap_ns_{max_recv_gap_ns}, max_ack_delay_ns_{max_ack_delay_ns} {}

    /// Starts monitoring a new connection: the connect counts as the first receive.
    void on_connect(int64_t now_ns) noexcept {
        monitoring_ = true;
        last_recv_ns_ = now_ns;
        has_unacked_ = false;
    }

    void on_recv(int64_t now_ns) noexcept {
        last_recv_ns_ = now_ns;
        has_unacked_ = false;
    }

    void on_send(int64_t now_ns) noexcept {
        if (!has_unacked_) {
            has_unacked_ = true;
            oldest_unacked_send_ns_ = now_ns;
        }
    }

    /// Stops monitoring (disconnected): is_alive() is true until the next on_connect().
    void reset() noexcept {
        monitoring_ = false;
        has_unacked_ = false;
    }

    [[nodiscard]] bool is_alive(int64_t now_ns) const noexcept {
        if (!monitoring_) return true;
        if (max_recv_gap_ns_ > 0 && now_ns - last_recv_ns_ > max_recv_gap_ns_) return false;
        if (max_ack_delay_ns_ > 0 && has_unacked_ &&
            now_ns - oldest_unacked_send_ns_ > max_ack_delay_ns_) {
            return false;
        }
        return true;
    }

    [[nodiscard]] int64_t last_recv_ns() const noexcept { return last_recv_ns_; }
    [[nodiscard]] bool has_unacked_send() const noexcept { return has_unacked_; }
    /// Meaningful only while has_unacked_send().
    [[nodiscard]] int64_t oldest_unacked_send_ns() const noexcept { return oldest_unacked_send_ns_; }

private:
    int64_t max_recv_gap_ns_;
    int64_t max_ack_delay_ns_;
    int64_t last_recv_ns_{0};
    int64_t oldest_unacked_send_ns_{0};
    bool monitoring_{false};
    bool has_unacked_{false};
};

} // namespace net

QUARK_END_NAMESPACE
