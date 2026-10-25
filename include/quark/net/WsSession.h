#pragma once

#include "quark/Attributes.h"
#include "quark/QuarkLog.h"
#include "quark/net/BufferPool.h"
#include "quark/net/Endpoint.h"
#include "quark/net/EventLoop.h"
#include "quark/net/ExpBackoff.h"
#include "quark/net/IOBuffer.h"
#include "quark/net/Stream.h"
#include "quark/net/WsFrame.h"
#include "quark/net/WsHandshake.h"
#include "quark/utils/InplaceFunction.h"
#include "quark/utils/RdtscClock.h"
#include "quark/utils/TimerQueue.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

QUARK_BEGIN_NAMESPACE

namespace net {

enum class WsState : uint8_t { Disconnected, Connecting, Upgrading, Open, Closing };
enum class WsSendStatus : uint8_t { Ok, WouldBlock, NotOpen, TooLarge };

/// A received text or binary message, valid only during the on_message callback.
struct WsMessage {
    static constexpr std::size_t kPadding = 64;

    WsOpcode opcode;
    std::span<const uint8_t> payload;
    std::size_t capacity;  ///< readable bytes from payload.data(): >= payload.size() + kPadding
    int64_t recv_ns;       ///< RdtscClock::tscns() right after the read that completed it

    [[nodiscard]] std::string_view text() const noexcept {
        return {reinterpret_cast<const char*>(payload.data()), payload.size()};
    }
};

struct WsSessionOptions {
    bool auto_reconnect{true};
    int64_t connect_timeout_ns{10'000'000'000};  ///< TCP + TLS + upgrade
    int64_t reconnect_initial_ns{500'000'000};
    int64_t reconnect_max_ns{30'000'000'000};
    int64_t stable_after_ns{30'000'000'000};     ///< uptime that resets the backoff
    int64_t ping_interval_ns{0};                 ///< 0: no client pings
    int64_t pong_timeout_ns{0};                  ///< 0: pings without a deadline
    int64_t max_recv_gap_ns{0};                  ///< 0: no receive gap check
};

/// WebSocket client (RFC 6455) over any Stream: WsSession<TcpSocket> is ws://, WsSession<TlsStream>
/// is wss://. Everything runs on the EventLoop's thread, including send() (e.g. from the loop's
/// step hook after draining an order queue) and the callbacks.
///
/// - Receive: frames are parsed in place in a buffer from `recv_pool` and handed out as Message
///   views (no copy). At least kPadding readable bytes always follow a payload, so parsers that
///   over-read (simdjson) can parse it where it is. Messages larger than a receive buffer fail
///   the connection: size `recv_pool` buffers for the largest expected message.
/// - Send: a frame is built in one `send_pool` buffer (header prepended into its headroom, payload
///   masked in place) and written right away; what the transport doesn't take waits in a bounded
///   queue. A full queue or pool returns SendStatus::WouldBlock: backpressure, never blocking.
/// - Handshake: a fresh random key per connection; anything but a 101 with the matching
///   Sec-WebSocket-Accept (e.g. a 429 or 418 from a rate-limiting exchange) fails the attempt.
/// - Reconnect (auto_reconnect): exponential backoff with jitter, reset only once a connection
///   has stayed up for stable_after_ns, so a server that accepts and drops at once isn't hammered.
/// - Keepalive: optional client pings with a pong timeout, and/or a maximum gap between received
///   bytes; either failing reconnects.
///
/// Callbacks may call send() and close(), but must not destroy the session.
template <Stream Transport>
class WsSession final : public Pollable {
public:
    static constexpr std::size_t kPadding = WsMessage::kPadding;
    static constexpr std::size_t kMaxPendingFrames = 64;

    // transport-independent types, so code driving sessions needn't be a template
    using State = WsState;
    using SendStatus = WsSendStatus;
    using Message = WsMessage;
    using Options = WsSessionOptions;

    using MessageHandler = InplaceFunction<void(const Message&), 64>;
    using StateHandler = InplaceFunction<void(State), 64>;

    WsSession(EventLoop& loop, BufferPool& recv_pool, BufferPool& send_pool)
        : WsSession(loop, recv_pool, send_pool, Options{}) {}

    WsSession(EventLoop& loop, BufferPool& recv_pool, BufferPool& send_pool, const Options& options)
        : loop_{loop},
          recv_pool_{recv_pool},
          send_pool_{send_pool},
          opts_{options},
          backoff_{options.reconnect_initial_ns, options.reconnect_max_ns, 0.2, os_random_u64()},
          mask_gen_{os_random_u64()} {}

    ~WsSession() override {
        user_closed_ = true;
        cancel_timer(reconnect_timer_);
        teardown(/*reconnect=*/false, /*notify=*/false);
        recv_pool_.release(recv_);
    }

    WsSession(const WsSession&) = delete;
    WsSession& operator=(const WsSession&) = delete;

    // ---------------------------------------------------------------- configuration (cold path)

    void on_message(MessageHandler h) { on_message_ = std::move(h); }
    void on_state(StateHandler h) { on_state_ = std::move(h); }
    /// Server name for the Host header (and SNI / certificate checks over TLS).
    void set_host(std::string_view host) { host_.assign(host); }
    /// Request target, e.g. "/ws" or "/stream?streams=btcusdt@trade".
    void set_target(std::string_view target) { target_.assign(target); }
    /// Extra upgrade request header, e.g. an API key some venues expect on connect.
    void add_header(std::string_view name, std::string_view value) {
        extra_headers_.append(name).append(": ").append(value).append("\r\n");
    }
    [[nodiscard]] Transport& transport() noexcept { return transport_; }

    // ---------------------------------------------------------------- control

    /// Connects (or reconnects right away) to `ep`; later automatic reconnects go to `ep` too.
    void connect(const Endpoint& ep) {
        ep_ = ep;
        user_closed_ = false;
        cancel_timer(reconnect_timer_);
        if (state_ != State::Disconnected) teardown(/*reconnect=*/false, /*notify=*/false);
        start_connect();
    }

    /// Sends a Close frame and disconnects; no automatic reconnect after this.
    void close(uint16_t code = 1000) {
        user_closed_ = true;
        cancel_timer(reconnect_timer_);
        if (state_ == State::Open) {
            const uint8_t body[2] = {static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code & 0xFF)};
            (void)enqueue_frame(WsOpcode::Close, body);
            set_state(State::Closing);
            if (pending_count_ == 0) teardown(false, true);
        } else if (state_ != State::Disconnected) {
            teardown(false, true);
        }
    }

    [[nodiscard]] SendStatus send_text(std::string_view text) {
        return send({reinterpret_cast<const uint8_t*>(text.data()), text.size()}, WsOpcode::Text);
    }
    [[nodiscard]] SendStatus send_binary(std::span<const uint8_t> data) { return send(data, WsOpcode::Binary); }

    [[nodiscard]] SendStatus send(std::span<const uint8_t> payload, WsOpcode opcode) {
        if (state_ != State::Open) [[unlikely]] return SendStatus::NotOpen;
        return enqueue_frame(opcode, payload);
    }

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool is_open() const noexcept { return state_ == State::Open; }
    [[nodiscard]] std::size_t pending_frames() const noexcept { return pending_count_; }
    /// HTTP status of the last upgrade response (101 when accepted, 0 if none was received).
    [[nodiscard]] int last_upgrade_status() const noexcept { return last_upgrade_status_; }
    /// Failed connection attempts since the backoff was last reset.
    [[nodiscard]] uint32_t reconnect_attempts() const noexcept { return backoff_.attempts(); }

    // ---------------------------------------------------------------- Pollable

    [[nodiscard]] int fd() const noexcept override { return transport_.fd(); }

    void on_ready(uint32_t /*flags*/) override {
        switch (state_) {
            case State::Connecting: {
                const StreamPhase ph = transport_.poll_open();
                if (ph == StreamPhase::Open) {
                    set_state(State::Upgrading);
                    send_upgrade_request();
                } else if (ph != StreamPhase::Connecting) {
                    QUARK_LOG_WARN("ws {}: connect to {} failed", host_, ep_.to_string());
                    teardown(true, true);
                }
                return;
            }
            case State::Upgrading:
                send_upgrade_request();
                if (registered_) read_input();
                return;
            case State::Open:
            case State::Closing:
                flush_pending();
                if (registered_) read_input();
                return;
            case State::Disconnected: return;
        }
    }

private:
    struct Pending {
        IOBuffer buf;
        std::size_t sent{0};
    };

    EventLoop& loop_;
    BufferPool& recv_pool_;
    BufferPool& send_pool_;
    Options opts_;
    Transport transport_;
    State state_{State::Disconnected};
    bool registered_{false};
    bool write_armed_{false};
    bool user_closed_{false};

    Endpoint ep_;
    std::string host_;
    std::string target_{"/"};
    std::string extra_headers_;
    WsHandshakeKey key_{};
    std::string request_;
    std::size_t request_off_{0};
    int last_upgrade_status_{0};

    IOBuffer recv_;
    IOBuffer frag_;
    WsOpcode frag_opcode_{WsOpcode::Text};
    int64_t recv_ns_{0};

    Pending pending_[kMaxPendingFrames];
    std::size_t pending_head_{0};
    std::size_t pending_count_{0};

    ExpBackoff backoff_;
    WsMaskKeyGen mask_gen_;
    int64_t opened_at_ns_{0};
    int64_t ping_sent_ns_{0};
    bool awaiting_pong_{false};

    TimerId connect_timer_{};
    TimerId reconnect_timer_{};
    TimerId health_timer_{};

    MessageHandler on_message_;
    StateHandler on_state_;

    // ---------------------------------------------------------------- connection lifecycle

    void start_connect() {
        if (!recv_ && !(recv_ = recv_pool_.acquire())) {
            QUARK_LOG_ERROR("ws {}: no receive buffer available", host_);
            schedule_reconnect();
            return;
        }
        recv_.reset();
        key_ = make_ws_handshake_key();
        request_.clear();
        request_off_ = 0;
        std::string host_header = host_;
        if (const uint16_t port = ntohs(ep_.port_be); port != 80 && port != 443) {
            host_header.append(":").append(std::to_string(port));
        }
        build_ws_upgrade_request(request_, host_header, target_, key_.key_view(), extra_headers_);

        if constexpr (requires(Transport& t, std::string_view h) { t.set_hostname(h); }) {
            transport_.set_hostname(host_);
        }
        if (!transport_.connect(ep_)) {
            schedule_reconnect();
            return;
        }
        if (!loop_.add(*this, /*want_write=*/true)) {
            QUARK_LOG_ERROR("ws {}: cannot register with the event loop", host_);
            transport_.close();
            schedule_reconnect();
            return;
        }
        registered_ = true;
        write_armed_ = true;  // a connecting socket reports completion as writable
        set_state(State::Connecting);
        if (opts_.connect_timeout_ns > 0) {
            connect_timer_ = loop_.timers().add_task("ws_connect_timeout", [this] {
                connect_timer_ = kInvalidTimerId;
                if (state_ == State::Connecting || state_ == State::Upgrading) {
                    QUARK_LOG_WARN("ws {}: connect to {} timed out", host_, ep_.to_string());
                    teardown(true, true);
                }
                return false;
            }, loop_.now_ns() + opts_.connect_timeout_ns);
        }
    }

    void send_upgrade_request() {
        while (request_off_ < request_.size()) {
            const std::span<const uint8_t> rest{reinterpret_cast<const uint8_t*>(request_.data()) + request_off_,
                                                request_.size() - request_off_};
            const sys::io_result n = transport_.write(rest, false);
            if (n == -EAGAIN) {
                set_write_interest(true);
                return;
            }
            if (n < 0) {
                teardown(true, true);
                return;
            }
            request_off_ += static_cast<std::size_t>(n);
        }
        set_write_interest(false);
    }

    void on_upgraded() {
        cancel_timer(connect_timer_);
        opened_at_ns_ = loop_.now_ns();
        awaiting_pong_ = false;
        set_state(State::Open);
        const int64_t period = opts_.ping_interval_ns > 0 ? opts_.ping_interval_ns : opts_.max_recv_gap_ns;
        if (period > 0 && state_ == State::Open) {
            health_timer_ = loop_.timers().add_task("ws_health", [this] { return check_health(); },
                                                    loop_.now_ns() + period, period);
        }
    }

    bool check_health() {
        const int64_t now = loop_.now_ns();
        if (opts_.max_recv_gap_ns > 0 && now - recv_ns_ > opts_.max_recv_gap_ns) {
            QUARK_LOG_WARN("ws {}: nothing received for {} ms, reconnecting", host_, (now - recv_ns_) / 1'000'000);
            teardown(true, true);
            return false;
        }
        if (opts_.ping_interval_ns > 0) {
            if (awaiting_pong_) {
                if (opts_.pong_timeout_ns > 0 && now - ping_sent_ns_ > opts_.pong_timeout_ns) {
                    QUARK_LOG_WARN("ws {}: pong timeout, reconnecting", host_);
                    teardown(true, true);
                    return false;
                }
            } else if (enqueue_frame(WsOpcode::Ping, {}) == SendStatus::Ok) {
                awaiting_pong_ = true;
                ping_sent_ns_ = now;
            }
        }
        return state_ == State::Open;
    }

    /// Closes the connection. `reconnect`: schedule an automatic reconnect (unless closed by the
    /// user); `notify`: report State::Disconnected.
    void teardown(bool reconnect, bool notify) {
        if (registered_) {
            loop_.remove(*this);  // before the fd is closed
            registered_ = false;
        }
        transport_.close();
        write_armed_ = false;
        while (pending_count_ > 0) {
            send_pool_.release(pending_[pending_head_].buf);
            pending_head_ = (pending_head_ + 1) % kMaxPendingFrames;
            --pending_count_;
        }
        recv_pool_.release(frag_);
        cancel_timer(connect_timer_);
        cancel_timer(health_timer_);
        awaiting_pong_ = false;
        const bool was_stable = opened_at_ns_ != 0 && loop_.now_ns() - opened_at_ns_ >= opts_.stable_after_ns;
        opened_at_ns_ = 0;
        if (was_stable) backoff_.reset();
        if (state_ == State::Disconnected) return;
        state_ = State::Disconnected;
        if (notify && on_state_) on_state_(State::Disconnected);
        // the callback may have connected again
        if (reconnect && state_ == State::Disconnected) schedule_reconnect();
    }

    void schedule_reconnect() {
        if (!opts_.auto_reconnect || user_closed_ || reconnect_timer_) return;
        const int64_t delay = backoff_.next_delay_ns();
        QUARK_LOG_INFO("ws {}: reconnecting to {} in {} ms (attempt {})", host_, ep_.to_string(), delay / 1'000'000,
                       backoff_.attempts());
        reconnect_timer_ = loop_.timers().add_task("ws_reconnect", [this] {
            reconnect_timer_ = kInvalidTimerId;
            if (state_ == State::Disconnected && !user_closed_) start_connect();
            return false;
        }, loop_.now_ns() + delay);
    }

    void cancel_timer(TimerId& id) noexcept {
        if (id) loop_.timers().cancel(id);
        id = kInvalidTimerId;
    }

    void set_state(State s) {
        if (s == state_) return;
        state_ = s;
        if (on_state_) on_state_(s);
    }

    void set_write_interest(bool want) noexcept {
        if (registered_ && write_armed_ != want) {
            loop_.set_write_interest(*this, want);
            write_armed_ = want;
        }
    }

    // ---------------------------------------------------------------- receive

    /// Readable span: the tail kPadding bytes of the buffer are never filled.
    [[nodiscard]] static std::span<uint8_t> readable_space(IOBuffer& b) noexcept {
        const std::span<uint8_t> spare = b.spare();
        return spare.size() > kPadding ? spare.first(spare.size() - kPadding) : std::span<uint8_t>{};
    }

    [[nodiscard]] static std::size_t capacity_from(IOBuffer& b, const uint8_t* p) noexcept {
        return static_cast<std::size_t>(b.data() + b.size() + b.tailroom() - p);
    }

    /// Reads until the transport has nothing more, which both busy-poll and edge-triggered
    /// modes rely on.
    void read_input() {
        for (;;) {
            std::span<uint8_t> space = readable_space(recv_);
            if (space.empty() && recv_.headroom() > 0) {
                recv_.compact();
                space = readable_space(recv_);
            }
            if (space.empty()) {
                QUARK_LOG_ERROR("ws {}: message larger than the {} byte receive buffer", host_, recv_.capacity());
                teardown(true, true);
                return;
            }
            const sys::io_result n = transport_.read(space);
            if (n == -EAGAIN) return;
            if (n <= 0) {
                if (n == 0) {
                    QUARK_LOG_INFO("ws {}: connection closed by peer", host_);
                } else {
                    QUARK_LOG_WARN("ws {}: read failed: {}", host_, std::strerror(static_cast<int>(-n)));
                }
                teardown(true, true);
                return;
            }
            recv_ns_ = RdtscClock::tscns();
            recv_.append(static_cast<std::size_t>(n));
            if (state_ == State::Upgrading) {
                if (!process_upgrade_response()) return;
            }
            parse_frames();
            if (!registered_) return;  // torn down while handling the input
        }
    }

    /// False if the connection was closed.
    bool process_upgrade_response() {
        const WsUpgradeResponse r = parse_ws_upgrade_response(recv_.bytes(), key_.accept_view());
        if (!r.complete) return true;
        last_upgrade_status_ = r.status;
        if (!r.accepted) {
            QUARK_LOG_WARN("ws {}: upgrade rejected, HTTP status {}", host_, r.status);
            teardown(true, true);
            return false;
        }
        recv_.consume(r.header_len);
        on_upgraded();
        return registered_;
    }

    void parse_frames() {
        while (state_ == State::Open || state_ == State::Closing) {
            const std::span<const uint8_t> buf = recv_.bytes();
            WsFrameHeader hdr;
            const std::size_t hlen = ws_parse_header(buf, hdr);
            if (hlen == 0) break;
            if (hlen == kWsParseError || hdr.masked) {  // servers must not mask (RFC 6455 5.1)
                QUARK_LOG_WARN("ws {}: protocol error in received frame", host_);
                teardown(true, true);
                return;
            }
            if (hdr.payload_len > recv_.capacity() - kPadding - hlen) {
                QUARK_LOG_ERROR("ws {}: {} byte frame larger than the {} byte receive buffer", host_, hdr.payload_len,
                                recv_.capacity());
                teardown(true, true);
                return;
            }
            const std::size_t total = hlen + static_cast<std::size_t>(hdr.payload_len);
            if (buf.size() < total) break;
            dispatch(hdr, buf.subspan(hlen, total - hlen));
            if (!registered_) return;
            recv_.consume(total);
        }
        if (recv_.empty()) recv_.reset();
    }

    void dispatch(const WsFrameHeader& hdr, std::span<const uint8_t> payload) {
        switch (hdr.opcode) {
            case WsOpcode::Text:
            case WsOpcode::Binary:
                if (frag_) {
                    protocol_error("data frame inside a fragmented message");
                } else if (hdr.fin) {
                    deliver(hdr.opcode, payload, capacity_from(recv_, payload.data()));
                } else if ((frag_ = recv_pool_.acquire())) {
                    frag_opcode_ = hdr.opcode;
                    append_fragment(payload, false);
                } else {
                    QUARK_LOG_ERROR("ws {}: no buffer to reassemble a fragmented message", host_);
                    teardown(true, true);
                }
                break;
            case WsOpcode::Continuation:
                if (!frag_) {
                    protocol_error("continuation frame without a fragmented message");
                } else {
                    append_fragment(payload, hdr.fin);
                }
                break;
            case WsOpcode::Ping:
                (void)enqueue_frame(WsOpcode::Pong, payload);
                break;
            case WsOpcode::Pong:
                awaiting_pong_ = false;
                break;
            case WsOpcode::Close:
                if (state_ == State::Open) (void)enqueue_frame(WsOpcode::Close, payload.first(payload.size() >= 2 ? 2 : 0));
                QUARK_LOG_INFO("ws {}: closed by server (code {})", host_,
                               payload.size() >= 2 ? (payload[0] << 8 | payload[1]) : 1005);
                teardown(!user_closed_, true);
                break;
            default: break;
        }
    }

    void append_fragment(std::span<const uint8_t> chunk, bool fin) {
        if (chunk.size() > readable_space(frag_).size()) {
            QUARK_LOG_ERROR("ws {}: fragmented message larger than the {} byte receive buffer", host_, frag_.capacity());
            teardown(true, true);
            return;
        }
        if (!chunk.empty()) std::memcpy(frag_.append(chunk.size()).data(), chunk.data(), chunk.size());
        if (fin) {
            deliver(frag_opcode_, frag_.bytes(), capacity_from(frag_, frag_.data()));
            recv_pool_.release(frag_);
        }
    }

    void deliver(WsOpcode op, std::span<const uint8_t> payload, std::size_t capacity) {
        if (on_message_) on_message_(Message{op, payload, capacity, recv_ns_});
    }

    void protocol_error(const char* what) {
        QUARK_LOG_WARN("ws {}: protocol error: {}", host_, what);
        teardown(true, true);
    }

    // ---------------------------------------------------------------- send

    SendStatus enqueue_frame(WsOpcode opcode, std::span<const uint8_t> payload) {
        const std::size_t hsize = ws_client_header_size(payload.size());
        if (hsize + payload.size() > send_pool_.buffer_size()) return SendStatus::TooLarge;
        if (pending_count_ == kMaxPendingFrames) return SendStatus::WouldBlock;
        IOBuffer b = send_pool_.acquire(hsize);
        if (!b) return SendStatus::WouldBlock;

        uint8_t* p = b.append(payload.size()).data();
        if (!payload.empty()) std::memcpy(p, payload.data(), payload.size());
        const uint32_t key = mask_gen_.next();
        uint8_t key_bytes[4];
        std::memcpy(key_bytes, &key, 4);
        ws_mask(p, payload.size(), key_bytes);
        (void)ws_encode_client_header(b.prepend(hsize), opcode, payload.size(), key_bytes, true);

        Pending& slot = pending_[(pending_head_ + pending_count_) % kMaxPendingFrames];
        slot.buf = b;
        slot.sent = 0;
        ++pending_count_;
        flush_pending();
        return SendStatus::Ok;
    }

    void flush_pending() {
        while (pending_count_ > 0) {
            Pending& p = pending_[pending_head_];
            const sys::io_result n = transport_.write({p.buf.data() + p.sent, p.buf.size() - p.sent}, false);
            if (n == -EAGAIN) {
                set_write_interest(true);
                return;
            }
            if (n < 0) {
                QUARK_LOG_WARN("ws {}: write failed", host_);
                teardown(true, true);
                return;
            }
            p.sent += static_cast<std::size_t>(n);
            if (p.sent < p.buf.size()) {
                set_write_interest(true);
                return;
            }
            send_pool_.release(p.buf);
            pending_head_ = (pending_head_ + 1) % kMaxPendingFrames;
            --pending_count_;
        }
        set_write_interest(false);
        if (state_ == State::Closing) teardown(false, true);
    }
};

} // namespace net

QUARK_END_NAMESPACE
