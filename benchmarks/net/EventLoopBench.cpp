// The cost of driving connections: an EventLoop step itself, and what busy-poll and epoll mode
// pay per idle connection (kernel sockets).
#include "quark/net/EventLoop.h"
#include "quark/net/TcpSocket.h"

#include <benchmark/benchmark.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <memory>
#include <vector>

using namespace quark;
using namespace quark::net;

namespace {

struct NoopPollable : Pollable {
    int fd_;
    explicit NoopPollable(int fd = -1) : fd_{fd} {}
    [[nodiscard]] int fd() const noexcept override { return fd_; }
    void on_ready(uint32_t) override { benchmark::ClobberMemory(); }
};

// a connected loopback TCP pair, nonblocking, nothing in flight
struct TcpPair {
    int client{-1}, server{-1};
    TcpPair() {
        const int lst = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(a);
        ::bind(lst, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::listen(lst, 1);
        ::getsockname(lst, reinterpret_cast<sockaddr*>(&a), &len);
        client = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        (void)::connect(client, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        server = ::accept(lst, nullptr, nullptr);
        ::close(lst);
    }
    ~TcpPair() {
        ::close(client);
        ::close(server);
    }
};

// busy-poll step with N registered pollables that do nothing: the loop's own overhead
void BM_EventLoopBusyStep(benchmark::State& state) {
    EventLoop loop({.busy_poll = true});
    std::vector<std::unique_ptr<NoopPollable>> ps;
    for (int64_t i = 0; i < state.range(0); ++i) {
        ps.push_back(std::make_unique<NoopPollable>());
        loop.add(*ps.back(), false);
    }
    for (auto _ : state) loop.step();
}
BENCHMARK(BM_EventLoopBusyStep)->Arg(0)->Arg(1)->Arg(16);

// what a busy-poll connection costs per step when nothing arrived: one recv() returning EAGAIN
void BM_IdleRecvSyscall(benchmark::State& state) {
    TcpPair pair;
    TcpSocket dummy;
    uint8_t buf[4096];
    for (auto _ : state) benchmark::DoNotOptimize(sys::recv(pair.client, buf, sizeof(buf)));
}
BENCHMARK(BM_IdleRecvSyscall);

// epoll mode with N idle connections registered: one epoll_wait(0) per step, whatever N is
void BM_EventLoopEpollIdleStep(benchmark::State& state) {
    EventLoop loop({.busy_poll = false});
    std::vector<std::unique_ptr<TcpPair>> pairs;
    std::vector<std::unique_ptr<NoopPollable>> ps;
    for (int64_t i = 0; i < state.range(0); ++i) {
        pairs.push_back(std::make_unique<TcpPair>());
        ps.push_back(std::make_unique<NoopPollable>(pairs.back()->client));
        loop.add(*ps.back(), false);
    }
    for (int i = 0; i < 4; ++i) loop.step();  // consume the initial writable/readable edges
    for (auto _ : state) loop.step();
}
BENCHMARK(BM_EventLoopEpollIdleStep)->Arg(1)->Arg(64);

}  // namespace
