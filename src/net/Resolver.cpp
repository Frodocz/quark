#include "quark/net/Resolver.h"

#include "quark/QuarkLog.h"

#include <chrono>
#include <utility>

QUARK_BEGIN_NAMESPACE

namespace net {

Resolver::Resolver(int64_t refresh_ns) : refresh_ns_{refresh_ns}, thread_{[this] { run(); }} {}

Resolver::~Resolver() {
    {
        std::lock_guard lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

std::size_t Resolver::watch(std::string_view host, uint16_t port) {
    std::vector<Endpoint> addrs = Endpoint::resolve_all(host, port);
    std::lock_guard lock(mu_);
    hosts_.push_back({std::string(host), port, std::move(addrs)});
    return hosts_.size() - 1;
}

std::vector<Endpoint> Resolver::endpoints(std::size_t id) const {
    std::lock_guard lock(mu_);
    return hosts_.at(id).addrs;
}

Endpoint Resolver::next(std::size_t id) {
    std::lock_guard lock(mu_);
    Host& h = hosts_.at(id);
    if (h.addrs.empty()) return {};
    return h.addrs[h.cursor++ % h.addrs.size()];
}

void Resolver::run() {
    std::unique_lock lock(mu_);
    while (!cv_.wait_for(lock, std::chrono::nanoseconds(refresh_ns_), [this] { return stop_; })) {
        std::vector<std::pair<std::string, uint16_t>> names;
        for (const Host& h : hosts_) names.emplace_back(h.name, h.port);
        lock.unlock();
        std::vector<std::vector<Endpoint>> results;
        for (const auto& [name, port] : names) results.push_back(Endpoint::resolve_all(name, port));
        lock.lock();
        for (std::size_t i = 0; i < results.size(); ++i) {
            if (results[i].empty()) continue;  // keep the last good addresses
            if (results[i] != hosts_[i].addrs) {
                QUARK_LOG_INFO("dns {}: {} addresses", hosts_[i].name, results[i].size());
                hosts_[i].addrs = std::move(results[i]);
            }
        }
    }
}

} // namespace net

QUARK_END_NAMESPACE
