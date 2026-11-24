#pragma once

#include "quark/Attributes.h"
#include "quark/net/Endpoint.h"
#include "quark/net/HttpClient.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

QUARK_BEGIN_NAMESPACE

namespace net {

struct HttpPoolOptions {
    std::size_t clients = 2;
    HttpClientOptions client{};
    std::size_t max_queued = 256;  ///< request() refuses more waiting requests than this
    /// A request still waiting for a client after this long fails; 0: client.request_timeout_ns.
    int64_t queue_timeout_ns = 0;
};

/// N keep-alive HTTP clients to one host, spread over its addresses, sharing a FIFO of requests:
/// each request goes to the first idle client, so slow responses don't hold up the others.
/// Everything runs on the EventLoop's thread.
template <Stream Transport>
class HttpPool {
public:
    using Client = HttpClient<Transport>;
    /// The response, or nullptr if the request failed (timeout, connection error).
    using Handler = std::function<void(const HttpResponseView*)>;
    using Addresses = std::function<std::vector<Endpoint>()>;

    HttpPool(EventLoop& loop, BufferPool& pool, const HttpPoolOptions& options) : loop_{loop}, opts_{options} {
        if (opts_.queue_timeout_ns == 0) opts_.queue_timeout_ns = opts_.client.request_timeout_ns;
        for (std::size_t i = 0; i < std::max<std::size_t>(1, opts_.clients); ++i) {
            auto& m = members_.emplace_back(std::make_unique<Member>(loop, pool, opts_.client));
            m->http.on_response([this, i](const HttpResponseView& r) { finish(i, &r); });
            m->http.on_error([this, i] { finish(i, nullptr); });
            m->http.set_endpoint_provider([this, i] { return address_for(i); });
        }
    }

    ~HttpPool() {
        if (retry_timer_) loop_.timers().cancel(retry_timer_);
    }

    HttpPool(const HttpPool&) = delete;
    HttpPool& operator=(const HttpPool&) = delete;

    /// Applies `fn(Client&)` to every client, e.g. to set the TLS context.
    template <class Fn>
    void configure(Fn&& fn) {
        for (auto& m : members_) fn(m->http);
    }
    void set_host(std::string_view h) { configure([&](Client& c) { c.set_host(h); }); }

    /// Connects every client, spread over `addresses()`; reconnects ask it again.
    void connect(Addresses addresses) {
        addresses_ = std::move(addresses);
        const std::vector<Endpoint> eps = addresses_();
        for (std::size_t i = 0; i < members_.size(); ++i) {
            members_[i]->ep = eps.empty() ? Endpoint{} : eps[i % eps.size()];
            members_[i]->http.connect(members_[i]->ep);
        }
    }

    void close() {
        for (auto& m : members_) m->http.close();
    }

    /// Queues a request (headers: complete lines without CRLF); false if the queue is full.
    bool request(HttpMethod method, std::string target, std::vector<std::string> headers, std::string body, Handler done) {
        if (queue_.size() >= opts_.max_queued) return false;
        queue_.push_back({method, std::move(target), std::move(headers), std::move(body), std::move(done), loop_.now_ns()});
        dispatch();
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return members_.size(); }
    [[nodiscard]] std::size_t queued() const noexcept { return queue_.size(); }
    /// Clients connected (idle or waiting for a response).
    [[nodiscard]] std::size_t connected_count() const noexcept {
        std::size_t n = 0;
        for (const auto& m : members_) n += m->http.ready() || m->busy;
        return n;
    }

private:
    struct Request {
        HttpMethod method;
        std::string target;
        std::vector<std::string> headers;
        std::string body;
        Handler done;
        int64_t queued_ns;
    };
    struct Member {
        Client http;
        Endpoint ep;
        bool busy = false;
        Handler done;
        Member(EventLoop& l, BufferPool& p, const HttpClientOptions& o) : http{l, p, o} {}
    };

    EventLoop& loop_;
    HttpPoolOptions opts_;
    std::vector<std::unique_ptr<Member>> members_;  // clients don't move: they're registered with the loop
    std::deque<Request> queue_;
    Addresses addresses_;
    TimerId retry_timer_{};

    void fail_front() {
        Handler done = std::move(queue_.front().done);
        queue_.pop_front();
        if (done) done(nullptr);
    }

    /// Sends queued requests on idle clients; while some wait for a (re)connecting client, retries every ms.
    void dispatch() {
        const int64_t now = loop_.now_ns();
        while (!queue_.empty() && opts_.queue_timeout_ns > 0 && now - queue_.front().queued_ns > opts_.queue_timeout_ns) {
            fail_front();
        }
        for (std::size_t i = 0; i < members_.size() && !queue_.empty(); ++i) {
            Member& m = *members_[i];
            if (m.busy || !m.http.ready()) continue;
            Request& r = queue_.front();
            std::vector<std::string_view> lines(r.headers.begin(), r.headers.end());
            const auto body = std::span(reinterpret_cast<const uint8_t*>(r.body.data()), r.body.size());
            const HttpSubmit s = m.http.request(r.method, r.target, lines, body);
            if (s == HttpSubmit::TooLarge) {
                fail_front();
                --i;  // the client is still idle
                continue;
            }
            if (s != HttpSubmit::Ok) continue;
            m.busy = true;
            m.done = std::move(r.done);
            queue_.pop_front();
        }
        if (!queue_.empty() && !retry_timer_) {
            retry_timer_ = loop_.timers().add_task("http_pool_dispatch", [this] {
                retry_timer_ = kInvalidTimerId;
                dispatch();
                return false;
            }, now + 1'000'000);
        }
    }

    void finish(std::size_t i, const HttpResponseView* r) {
        Member& m = *members_[i];
        if (!m.busy) return;  // an idle connection dropped: nothing was waiting on it
        m.busy = false;
        Handler done = std::move(m.done);
        if (done) done(r);
        dispatch();
    }

    Endpoint address_for(std::size_t i) {
        if (!addresses_) return {};
        const std::vector<Endpoint> eps = addresses_();
        Member& m = *members_[i];
        if (!eps.empty() && std::find(eps.begin(), eps.end(), m.ep) == eps.end()) m.ep = eps[i % eps.size()];
        return m.ep;
    }
};

} // namespace net

QUARK_END_NAMESPACE
