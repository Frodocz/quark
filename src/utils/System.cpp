#include "quark/utils/System.h"

#include "quark/QuarkLog.h"

#include <execinfo.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstring>
#include <fstream>
#include <string>

QUARK_BEGIN_NAMESPACE

// ---------------------------------------------------------------- threads and CPUs

bool cpu_list_contains(std::string_view list, int cpu) noexcept {
    // comma-separated CPUs or ranges, e.g. "0-3,8,10-11"
    while (!list.empty()) {
        const auto comma = list.find(',');
        std::string_view item = list.substr(0, comma);
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
        while (!item.empty() && (item.back() == '\n' || item.back() == ' ')) item.remove_suffix(1);
        if (item.empty()) continue;
        const auto dash = item.find('-');
        auto parse = [](std::string_view s) {
            int v = 0;
            for (const char c : s) {
                if (c < '0' || c > '9') return -1;
                v = v * 10 + (c - '0');
            }
            return s.empty() ? -1 : v;
        };
        const int lo = parse(item.substr(0, dash));
        const int hi = dash == std::string_view::npos ? lo : parse(item.substr(dash + 1));
        if (lo >= 0 && hi >= lo && cpu >= lo && cpu <= hi) return true;
    }
    return false;
}

bool cpu_isolated(int cpu) {
    std::ifstream f("/sys/devices/system/cpu/isolated");
    std::string list;
    std::getline(f, list);
    return cpu_list_contains(list, cpu);
}

bool pin_thread_to_cpu(int cpu, pthread_t thread) {
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
        QUARK_LOG_ERROR("pin_thread_to_cpu: invalid cpu {}", cpu);
        return false;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (const int rc = ::pthread_setaffinity_np(thread, sizeof(set), &set); rc != 0) {
        QUARK_LOG_ERROR("pin_thread_to_cpu {} failed: {}", cpu, std::strerror(rc));
        return false;
    }
    if (!cpu_isolated(cpu)) {
        QUARK_LOG_WARN("pinned to cpu {}, which isn't isolated (isolcpus): other tasks may preempt it", cpu);
    } else {
        QUARK_LOG_INFO("pinned to isolated cpu {}", cpu);
    }
    return true;
}

bool set_thread_realtime(int priority, pthread_t thread) {
    if (priority < 1 || priority > 99) {
        QUARK_LOG_ERROR("set_thread_realtime: priority {} not in 1..99", priority);
        return false;
    }
    sched_param param{};
    param.sched_priority = priority;
    if (const int rc = ::pthread_setschedparam(thread, SCHED_FIFO, &param); rc != 0) {
        QUARK_LOG_ERROR("set_thread_realtime {} failed: {}", priority, std::strerror(rc));
        return false;
    }
    return true;
}

bool set_thread_name(std::string_view name, pthread_t thread) {
    char buf[16];  // 15 characters + NUL: the kernel limit
    const std::size_t n = name.size() < sizeof(buf) - 1 ? name.size() : sizeof(buf) - 1;
    std::memcpy(buf, name.data(), n);
    buf[n] = '\0';
    return ::pthread_setname_np(thread, buf) == 0;
}

int current_cpu() noexcept { return ::sched_getcpu(); }

// ---------------------------------------------------------------- memory

bool lock_all_memory() {
    if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        QUARK_LOG_ERROR("lock_all_memory: mlockall failed: {}", std::strerror(errno));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- signals

namespace {

std::atomic<bool> g_shutdown{false};

void write_stderr(const char* s) noexcept {
    std::size_t len = 0;
    while (s[len] != '\0') ++len;
    while (len > 0) {
        const ssize_t n = ::write(STDERR_FILENO, s, len);
        if (n <= 0) return;
        s += n;
        len -= static_cast<std::size_t>(n);
    }
}

extern "C" void on_crash(int sig) {
    // async-signal-safe only: write(), backtrace() (pre-loaded), backtrace_symbols_fd()
    write_stderr("\n*** quark: fatal signal ");
    write_stderr(sigabbrev_np(sig) != nullptr ? sigabbrev_np(sig) : "?");
    write_stderr(", backtrace:\n");
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, n, STDERR_FILENO);
    std::signal(sig, SIG_DFL);  // default action (core dump) on the re-raise
    ::raise(sig);
}

extern "C" void on_shutdown(int sig) {
    if (g_shutdown.exchange(true)) {  // second request: stop now
        std::signal(sig, SIG_DFL);
        ::raise(sig);
    }
}

void install(int sig, void (*handler)(int), int flags) {
    struct sigaction sa{};
    sa.sa_handler = handler;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    if (::sigaction(sig, &sa, nullptr) != 0) {
        QUARK_LOG_ERROR("sigaction({}) failed: {}", sig, std::strerror(errno));
    }
}

}  // namespace

void install_crash_handler() {
    // backtrace() loads libgcc lazily on first use, which isn't safe inside a handler: do it now
    void* frame[1];
    (void)::backtrace(frame, 1);
    for (const int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) install(sig, &on_crash, SA_RESETHAND | SA_NODEFER);
}

void install_shutdown_handler() {
    for (const int sig : {SIGINT, SIGTERM}) install(sig, &on_shutdown, SA_RESTART);
}

bool shutdown_requested() noexcept { return g_shutdown.load(std::memory_order_relaxed); }

QUARK_END_NAMESPACE
