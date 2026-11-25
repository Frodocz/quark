#pragma once

#include "quark/Attributes.h"
#include "quark/QuarkError.h"
#include "quark/QuarkLog.h"
#include "quark/net/BufferPool.h"
#include "quark/net/Endpoint.h"
#include "quark/net/EventLoop.h"
#include "quark/net/ExpBackoff.h"
#include "quark/net/HttpCodec.h"
#include "quark/net/IOBuffer.h"
#include "quark/net/Stream.h"
#include "quark/net/WsHandshake.h"
#include "quark/utils/InplaceFunction.h"
#include "quark/utils/TimerQueue.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

QUARK_BEGIN_NAMESPACE

namespace net {

enum class HttpClientState : uint8_t { Disconnected, Connecting, Idle, Busy };
enum class HttpSubmit : uint8_t { Ok, Busy, NotReady, TooLarge };

struct HttpClientOptions {
    bool auto_reconnect{true};
    int64_t connect_timeout_ns{10'000'000'000};
    int64_t request_timeout_ns{5'000'000'000};
    int64_t reconnect_initial_ns{500'000'000};
    int64_t reconnect_max_ns{30'000'000'000};
    int64_t stable_after_ns{30'000'000'000};
};

/// Keep-alive HTTP/1.1 client over any Stream (HttpClient<TlsStream> is https://), one request
/// in flight at a time; use several clients for concurrency. Runs on the EventLoop's thread.
///
/// - Two buffers from `pool`: requests are built in one, responses parsed in place in the other
///   (size them for the largest response, e.g. an order book snapshot).
/// - A keep-alive connection the server closes while idle is noticed right away and reopened
///   (auto_reconnect) at once, without backoff once it has served a response, instead of failing
///   the next request.
/// - request() returning Ok means exactly one of on_response / on_error follows, later, from the
///   loop: never from inside request(), even when the connection fails while it sends.
/// - Without auto_reconnect, a failed connect or connect timeout reports on_error (nothing else
///   will happen); with it the client keeps retrying, with backoff.
/// - Connection: close responses, malformed responses, transport errors and timeouts drop the
///   connection; the last three also report on_error. After a Connection: close response the
///   client reconnects right away (no backoff) and is NotReady inside that on_response.
/// - A response without Content-Length or chunked encoding ends when the server closes the
///   connection (over TLS, a close without close_notify can't be told apart from truncation).
///
/// Callbacks may call request(), close() and connect(), but must not destroy the client.
template <Stream Transport>
class HttpClient final : public Pollable {
public:
    using State = HttpClientState;
    using Submit = HttpSubmit;
    using Options = HttpClientOptions;

    using ResponseHandler = InplaceFunction<void(const HttpResponseView&), 64>;
    using ErrorHandler = InplaceFunction<void(), 64>;

    HttpClient(EventLoop& loop, BufferPool& pool) : HttpClient(loop, pool, Options{}) {}

    HttpClient(EventLoop& loop, BufferPool& pool, const Options& options)
        : loop_{loop}, pool_{pool}, opts_{checked(options)},
          backoff_{options.reconnect_initial_ns, options.reconnect_max_ns, 0.2, os_random_u64()} {}

    ~HttpClient() override {
        user_closed_ = true;
        cancel_timer(reconnect_timer_);
        cancel_timer(error_timer_);
        teardown(false);
        pool_.release(send_);
        pool_.release(recv_);
    }

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    void on_response(ResponseHandler h) { on_response_ = std::move(h); }
    void on_error(ErrorHandler h) { on_error_ = std::move(h); }
    /// Host header value (and SNI / certificate checks over TLS).
    void set_host(std::string_view host) { host_.assign(host); }
    /// Asked for the address before every automatic reconnect (e.g. Resolver::rotating()); an
    /// invalid Endpoint keeps the previous one.
    void set_endpoint_provider(EndpointProvider p) { endpoint_provider_ = std::move(p); }
    [[nodiscard]] Transport& transport() noexcept { return transport_; }

    QUARK_NOINLINE QUARK_ATTRIBUTE_COLD void connect(const Endpoint& ep) {
        ep_ = ep;
        user_closed_ = false;
        cancel_timer(reconnect_timer_);
        if (state_ != State::Disconnected) teardown(false);
        start_connect();
    }

    QUARK_NOINLINE QUARK_ATTRIBUTE_COLD void close() {
        user_closed_ = true;
        cancel_timer(reconnect_timer_);
        teardown(false);
    }

    /// Sends a request if the client is idle. `header_lines` are complete lines without CRLF.
    [[nodiscard]] QUARK_ATTRIBUTE_HOT Submit request(HttpMethod method, std::string_view target,
                                                     std::span<const std::string_view> header_lines,
                                                     std::span<const uint8_t> body = {}) {
        if (state_ != State::Idle) return state_ == State::Busy ? Submit::Busy : Submit::NotReady;
        send_.reset();
        const std::span<uint8_t> spare = send_.spare();
        const std::size_t n = build_http_request({reinterpret_cast<char*>(spare.data()), spare.size()}, method, host_,
                                                 target, header_lines, body);
        if (n == 0) return Submit::TooLarge;
        send_.append(n);
        send_off_ = 0;
        recv_.reset();
        parse_ = {};
        state_ = State::Busy;
        if (opts_.request_timeout_ns > 0) {
            request_timer_ = loop_.timers().add_task("http_request_timeout", [this] {
                request_timer_ = kInvalidTimerId;
                if (state_ == State::Busy) {
                    QUARK_LOG_WARN("http {}: request timed out", host_);
                    fail();
                }
                return false;
            }, loop_.now_ns() + opts_.request_timeout_ns);
        }
        in_request_ = true;  // a failure while sending is reported after this returns Ok
        flush_send();
        in_request_ = false;
        return Submit::Ok;
    }

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool ready() const noexcept { return state_ == State::Idle; }

    [[nodiscard]] int fd() const noexcept override { return transport_.fd(); }

    QUARK_ATTRIBUTE_HOT void on_ready(uint32_t /*flags*/) override {
        switch (state_) {
            case State::Connecting: {
                const StreamPhase ph = transport_.poll_open();
                if (ph == StreamPhase::Open) {
                    cancel_timer(connect_timer_);
                    opened_at_ns_ = loop_.now_ns();
                    state_ = State::Idle;
                    set_write_interest(false);
                } else if (ph != StreamPhase::Connecting) {
                    QUARK_LOG_WARN("http {}: connect to {} failed", host_, ep_.to_string());
                    connect_failed();
                }
                return;
            }
            case State::Busy:
                flush_send();
                if (state_ == State::Busy && send_off_ == send_.size()) drive_recv();
                return;
            case State::Idle:
                check_idle_connection();
                return;
            case State::Disconnected: return;
        }
    }

private:
    EventLoop& loop_;
    BufferPool& pool_;
    Options opts_;
    Transport transport_;
    State state_{State::Disconnected};
    bool registered_{false};
    bool write_armed_{false};
    bool user_closed_{false};
    uint32_t conn_gen_{0};  // bumped by every teardown: callbacks may end or replace the connection
    Endpoint ep_;
    std::string host_;
    IOBuffer send_;
    std::size_t send_off_{0};
    IOBuffer recv_;
    HttpParseState parse_;  // where parsing of the response in recv_ stopped
    ExpBackoff backoff_;
    bool in_request_{false};  // inside request(): on_error is deferred until it has returned
    bool served_{false};      // this connection completed a response (an idle close is then routine)
    int64_t opened_at_ns_{0};
    TimerId connect_timer_{};
    TimerId request_timer_{};
    TimerId reconnect_timer_{};
    TimerId error_timer_{};  // a deferred on_error
    ResponseHandler on_response_;
    ErrorHandler on_error_;
    EndpointProvider endpoint_provider_;

    // Checked in every build type (ExpBackoff only assert()s them): a zero initial delay would
    // reconnect every nanosecond. Throws QuarkError (fatal with QUARK_NO_EXCEPTIONS) at construction.
    static const Options& checked(const Options& o) {
        QUARK_ASSERT(o.reconnect_initial_ns > 0 && o.reconnect_max_ns >= o.reconnect_initial_ns);
        return o;
    }

    QUARK_NOINLINE QUARK_ATTRIBUTE_COLD void start_connect() {
        if ((!send_ && !(send_ = pool_.acquire())) || (!recv_ && !(recv_ = pool_.acquire()))) {
            QUARK_LOG_ERROR("http {}: no buffers available", host_);
            connect_failed(/*deferred=*/true);
            return;
        }
        if constexpr (requires(Transport& t, std::string_view h) { t.set_hostname(h); }) {
            transport_.set_hostname(host_);
        }
        if (!transport_.connect(ep_) || !loop_.add(*this, true)) {
            transport_.close();
            connect_failed(/*deferred=*/true);  // may run inside connect(): report after it returns
            return;
        }
        registered_ = true;
        write_armed_ = true;
        state_ = State::Connecting;
        if (opts_.connect_timeout_ns > 0) {
            connect_timer_ = loop_.timers().add_task("http_connect_timeout", [this] {
                connect_timer_ = kInvalidTimerId;
                if (state_ == State::Connecting) {
                    QUARK_LOG_WARN("http {}: connect to {} timed out", host_, ep_.to_string());
                    connect_failed();
                }
                return false;
            }, loop_.now_ns() + opts_.connect_timeout_ns);
        }
    }

    QUARK_NOINLINE QUARK_ATTRIBUTE_COLD void reconnect() {
        if (endpoint_provider_) {
            if (const Endpoint e = endpoint_provider_(); e.valid()) ep_ = e;
        }
        start_connect();
    }

    void flush_send() {
        while (send_off_ < send_.size()) {
            const sys::io_result n = transport_.write({send_.data() + send_off_, send_.size() - send_off_}, false);
            if (n == -EAGAIN) {
                set_write_interest(true);
                return;
            }
            if (n < 0) {
                fail();
                return;
            }
            send_off_ += static_cast<std::size_t>(n);
        }
        // TLS may still hold records the socket didn't take: keep write interest until they're out
        const sys::io_result f = transport_.flush();
        if (f < 0 && f != -EAGAIN) {
            fail();
            return;
        }
        set_write_interest(f == -EAGAIN);
    }

    QUARK_ATTRIBUTE_HOT void drive_recv() {
        for (;;) {
            const std::span<uint8_t> space = recv_.spare();
            if (space.empty()) {
                // full: a close-delimited body may end exactly here, which only the close tells
                if (parse_http_response(recv_.bytes(), &parse_).close_delimited) {
                    uint8_t probe;
                    const sys::io_result p = transport_.read({&probe, 1});
                    if (p == -EAGAIN) return;
                    if (p == 0) {
                        HttpResponseView r = parse_http_response(recv_.bytes(), &parse_);
                        r.complete = true;
                        r.consumed = recv_.size();
                        complete_response(r);
                        return;
                    }
                }
                QUARK_LOG_ERROR("http {}: response larger than the {} byte buffer", host_, recv_.capacity());
                fail();
                return;
            }
            const sys::io_result n = transport_.read(space);
            if (n == -EAGAIN) return;
            loop_.note_work();
            if (n <= 0) {
                if (n == 0) {
                    HttpResponseView r = parse_http_response(recv_.bytes(), &parse_);
                    if (r.close_delimited) {  // its end is this close
                        r.complete = true;
                        r.consumed = recv_.size();
                        complete_response(r);
                        return;
                    }
                }
                QUARK_LOG_WARN("http {}: connection closed during the response", host_);
                fail();
                return;
            }
            recv_.append(static_cast<std::size_t>(n));
            const HttpResponseView r = parse_http_response(recv_.bytes(), &parse_);
            if (!r.complete) continue;
            if (r.malformed) {
                QUARK_LOG_WARN("http {}: malformed response", host_);
                fail();
                return;
            }
            complete_response(r);
            return;
        }
    }

    void complete_response(const HttpResponseView& r) {
        cancel_timer(request_timer_);
        if (!r.keep_alive) {
            // done with this connection before the callback: a request it submits gets NotReady
            // instead of going out on a socket about to close. A clean close is no failure.
            backoff_.reset();
            teardown(false);
        } else {
            state_ = State::Idle;
            served_ = true;
        }
        const uint32_t gen = conn_gen_;
        if (on_response_) on_response_(r);  // r.body is valid during the callback only
        if (gen != conn_gen_) return;      // closed or reconnected in the callback
        if (!r.keep_alive) {
            if (opts_.auto_reconnect && !user_closed_) reconnect();
        } else if (state_ == State::Idle) {
            recv_.reset();
            parse_ = {};
        }
    }

    // an idle keep-alive connection must stay silent: EOF means the server dropped it
    void check_idle_connection() {
        uint8_t probe[64];
        const sys::io_result n = transport_.read(probe);
        if (n == -EAGAIN) return;
        if (n == 0 && served_) {
            // the server's keep-alive timeout: routine, reopen right away (no backoff growth)
            QUARK_LOG_INFO("http {}: idle connection closed by server", host_);
            backoff_.reset();
            teardown(false);
            if (opts_.auto_reconnect && !user_closed_) reconnect();
            return;
        }
        if (n == 0) {
            QUARK_LOG_INFO("http {}: idle connection closed by server before any response", host_);
        } else {
            QUARK_LOG_WARN("http {}: unexpected data on an idle connection", host_);
        }
        teardown(true);
    }

    QUARK_NOINLINE QUARK_ATTRIBUTE_COLD void fail() {
        teardown(true);
        report_error(/*deferred=*/in_request_);
    }

    // Without auto_reconnect nothing else will happen after this: tell the user.
    QUARK_NOINLINE QUARK_ATTRIBUTE_COLD void connect_failed(bool deferred = false) {
        teardown(true);
        if (!opts_.auto_reconnect && !user_closed_) report_error(deferred);
    }

    // `deferred`: from inside a call of the user's (request(), connect()), which must return first
    void report_error(bool deferred) {
        if (!deferred) {
            if (on_error_) on_error_();
            return;
        }
        if (error_timer_) return;  // one is pending already
        error_timer_ = loop_.timers().add_task("http_error", [this] {
            error_timer_ = kInvalidTimerId;
            if (on_error_) on_error_();
            return false;
        }, loop_.now_ns());
    }

    QUARK_NOINLINE QUARK_ATTRIBUTE_COLD void teardown(bool reconnect) {
        ++conn_gen_;
        if (registered_) {
            loop_.remove(*this);
            registered_ = false;
        }
        transport_.close();
        write_armed_ = false;
        cancel_timer(connect_timer_);
        cancel_timer(request_timer_);
        if (opened_at_ns_ != 0 && loop_.now_ns() - opened_at_ns_ >= opts_.stable_after_ns) backoff_.reset();
        opened_at_ns_ = 0;
        served_ = false;
        state_ = State::Disconnected;
        if (reconnect) schedule_reconnect();
    }

    QUARK_NOINLINE QUARK_ATTRIBUTE_COLD void schedule_reconnect() {
        if (!opts_.auto_reconnect || user_closed_ || reconnect_timer_) return;
        const int64_t delay = backoff_.next_delay_ns();
        reconnect_timer_ = loop_.timers().add_task("http_reconnect", [this] {
            reconnect_timer_ = kInvalidTimerId;
            if (state_ == State::Disconnected && !user_closed_) reconnect();
            return false;
        }, loop_.now_ns() + delay);
    }

    void cancel_timer(TimerId& id) noexcept {
        if (id) loop_.timers().cancel(id);
        id = kInvalidTimerId;
    }

    void set_write_interest(bool want) noexcept {
        if (registered_ && write_armed_ != want) {
            loop_.set_write_interest(*this, want);
            write_armed_ = want;
        }
    }
};

} // namespace net

QUARK_END_NAMESPACE
