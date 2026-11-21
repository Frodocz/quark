#pragma once

#include "quark/Attributes.h"
#include "quark/QuarkLog.h"
#include "quark/net/Endpoint.h"
#include "quark/net/WsSession.h"
#include "quark/utils/InplaceFunction.h"
#include "quark/utils/TimeScopedDedup.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

QUARK_BEGIN_NAMESPACE

namespace net {

struct WsGroupOptions {
    std::size_t sessions = 2;
    WsSessionOptions session{};          ///< every session's (liveness presets included)
    bool dedup = true;                   ///< deliver only the first copy of a message
    int64_t dedup_window_ns = 10'000'000'000;
    std::size_t dedup_capacity = 1 << 16;
    /// How often the slowest session (by its delay behind the first copies) moves to an address
    /// no session uses; 0: never. Only with dedup, and only once every session has warmed up.
    int64_t rotate_interval_ns = 60'000'000'000;
    int64_t warmup_ns = 30'000'000'000;  ///< a session's delay counts after it has been open this long
    double delay_ema_alpha = 0.01;
};

/// N WebSocket sessions to one host, each on its own address where the host has enough of them.
/// - Market data (dedup): every session subscribes the same streams; each message is delivered
///   once, from whichever session got it first, and late copies measure each session's delay.
///   The slowest session is periodically reconnected elsewhere: first-arrival racing across IPs.
/// - Order entry (no dedup): send_best() uses the open session with the lowest ping RTT; every
///   session's messages are delivered.
/// Everything runs on the EventLoop's thread.
template <Stream Transport>
class WsGroup {
public:
    using Session = WsSession<Transport>;
    using MessageHandler = InplaceFunction<void(const WsMessage&, std::size_t session), 64>;
    using SessionHandler = InplaceFunction<void(std::size_t session), 64>;
    /// The host's current addresses, e.g. [&dns, id] { return dns.endpoints(id); }.
    using Addresses = std::function<std::vector<Endpoint>()>;

    WsGroup(EventLoop& loop, BufferPool& recv_pool, BufferPool& send_pool, const WsGroupOptions& options)
        : loop_{loop}, opts_{options}, dedup_{options.dedup_window_ns, options.dedup_capacity} {
        for (std::size_t i = 0; i < std::max<std::size_t>(1, opts_.sessions); ++i) {
            auto& m = members_.emplace_back(std::make_unique<Member>(loop, recv_pool, send_pool, opts_.session));
            m->ws.on_message([this, i](const WsMessage& msg) { deliver(i, msg); });
            m->ws.on_state([this, i](WsState s) {
                Member& mem = *members_[i];
                if (s == WsState::Open) {
                    mem.opened_ns = loop_.now_ns();
                    mem.delay_ns = 0;
                    if (on_open_) on_open_(i);
                } else if (s == WsState::Disconnected && on_close_) {
                    on_close_(i);
                }
            });
            m->ws.set_endpoint_provider([this, i] { return address_for(i); });
        }
    }

    ~WsGroup() { cancel_rotation(); }

    WsGroup(const WsGroup&) = delete;
    WsGroup& operator=(const WsGroup&) = delete;

    /// Applies `fn(Session&)` to every session, e.g. to set the TLS context.
    template <class Fn>
    void configure(Fn&& fn) {
        for (auto& m : members_) fn(m->ws);
    }
    void set_host(std::string_view h) { configure([&](Session& s) { s.set_host(h); }); }
    void set_target(std::string_view t) { configure([&](Session& s) { s.set_target(t); }); }
    void add_header(std::string_view n, std::string_view v) { configure([&](Session& s) { s.add_header(n, v); }); }

    void on_message(MessageHandler h) { on_message_ = std::move(h); }
    /// A session opened (send its subscriptions here) or dropped.
    void on_open(SessionHandler h) { on_open_ = std::move(h); }
    void on_close(SessionHandler h) { on_close_ = std::move(h); }

    /// Connects every session, spread over `addresses()`; reconnects ask it again.
    void connect(Addresses addresses) {
        addresses_ = std::move(addresses);
        const std::vector<Endpoint> eps = addresses_();
        for (std::size_t i = 0; i < members_.size(); ++i) {
            members_[i]->ep = eps.empty() ? Endpoint{} : eps[i % eps.size()];
            members_[i]->ws.connect(members_[i]->ep);
        }
        if (opts_.dedup && opts_.rotate_interval_ns > 0 && members_.size() > 1 && !rotate_timer_) {
            rotate_timer_ = loop_.timers().add_task("ws_group_rotate", [this] {
                rotate();
                return true;
            }, loop_.now_ns() + opts_.rotate_interval_ns, opts_.rotate_interval_ns);
        }
    }

    void close() {
        cancel_rotation();
        for (auto& m : members_) m->ws.close();
    }

    using SendStatus = WsSendStatus;
    SendStatus send(std::size_t session, std::string_view text) { return members_[session]->ws.send_text(text); }

    /// On the open session with the lowest ping RTT (unmeasured ones last); NotOpen if none is.
    SendStatus send_best(std::string_view text) {
        Session* best = nullptr;
        int64_t best_rtt = std::numeric_limits<int64_t>::max();
        for (auto& m : members_) {
            if (!m->ws.is_open()) continue;
            const int64_t rtt = m->ws.last_rtt_ns() > 0 ? m->ws.last_rtt_ns() : std::numeric_limits<int64_t>::max() - 1;
            if (best == nullptr || rtt < best_rtt) {
                best = &m->ws;
                best_rtt = rtt;
            }
        }
        return best == nullptr ? SendStatus::NotOpen : best->send_text(text);
    }

    [[nodiscard]] std::size_t size() const noexcept { return members_.size(); }
    [[nodiscard]] std::size_t open_count() const noexcept {
        return static_cast<std::size_t>(std::count_if(members_.begin(), members_.end(), [](const auto& m) { return m->ws.is_open(); }));
    }
    [[nodiscard]] Session& session(std::size_t i) { return members_[i]->ws; }
    [[nodiscard]] const Endpoint& address(std::size_t i) const { return members_[i]->ep; }
    /// Average delay of a session's copies behind the first ones (dedup only).
    [[nodiscard]] double delay_ns(std::size_t i) const { return members_[i]->delay_ns; }

private:
    struct Member {
        Session ws;
        Endpoint ep;
        int64_t opened_ns = 0;
        double delay_ns = 0;
        Member(EventLoop& l, BufferPool& r, BufferPool& s, const WsSessionOptions& o) : ws{l, r, s, o} {}
    };

    EventLoop& loop_;
    WsGroupOptions opts_;
    TimeScopedDedup dedup_;
    std::vector<std::unique_ptr<Member>> members_;  // sessions don't move: they're registered with the loop
    Addresses addresses_;
    MessageHandler on_message_;
    SessionHandler on_open_;
    SessionHandler on_close_;
    TimerId rotate_timer_{};

    void deliver(std::size_t i, const WsMessage& m) {
        if (!opts_.dedup) {
            if (on_message_) on_message_(m, i);
            return;
        }
        const auto seen = dedup_.first(std::hash<std::string_view>{}(m.text()), m.recv_ns);
        Member& mem = *members_[i];
        mem.delay_ns += opts_.delay_ema_alpha * (static_cast<double>(m.recv_ns - seen.first_ns) - mem.delay_ns);
        if (seen.first && on_message_) on_message_(m, i);
    }

    /// The session's address, kept across reconnects while the host still has it.
    Endpoint address_for(std::size_t i) {
        if (!addresses_) return {};
        const std::vector<Endpoint> eps = addresses_();
        Member& m = *members_[i];
        if (!eps.empty() && std::find(eps.begin(), eps.end(), m.ep) == eps.end()) m.ep = eps[i % eps.size()];
        return m.ep;
    }

    void rotate() {
        const int64_t now = loop_.now_ns();
        Member* slowest = nullptr;
        for (auto& m : members_) {
            if (!m->ws.is_open() || now - m->opened_ns < opts_.warmup_ns) return;  // judge only warmed-up groups
            if (slowest == nullptr || m->delay_ns > slowest->delay_ns) slowest = m.get();
        }
        if (slowest == nullptr || !addresses_) return;
        for (const Endpoint& ep : addresses_()) {
            const bool used = std::any_of(members_.begin(), members_.end(), [&](const auto& m) { return m->ep == ep; });
            if (used) continue;
            QUARK_LOG_INFO("ws group: moving the slowest session ({} us behind) from {} to {}",
                           static_cast<int64_t>(slowest->delay_ns / 1000), slowest->ep.to_string(), ep.to_string());
            slowest->ep = ep;
            slowest->ws.connect(ep);
            return;
        }
    }

    void cancel_rotation() {
        if (rotate_timer_) loop_.timers().cancel(rotate_timer_);
        rotate_timer_ = kInvalidTimerId;
    }
};

} // namespace net

QUARK_END_NAMESPACE
