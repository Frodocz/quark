// quark::fatal_error(), the crash handlers installed by init_log() and the log flush on the way
// down. Every scenario runs in a death-test child (a function below, so commas in its body don't
// split the gtest macro's arguments); the parent then reads the log file the child wrote, which
// proves the message reached the disk before the process died.
#include "quark/QuarkConfig.h"
#include "quark/QuarkError.h"
#include "quark/QuarkLog.h"
#include "quark/utils/RdtscClock.h"

#include <gtest/gtest.h>

#include "QuarkTest.h"

#include <signal.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

using namespace quark;
namespace fs = std::filesystem;

namespace {

// A fresh directory per test; also run (identically) by the re-executed death-test child.
fs::path fresh_dir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / (std::string{"quark_fatal_test_"} + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// Everything the child logged, whatever the start-time suffix of the file names.
std::string read_logs(const fs::path& dir) {
    std::string all;
    for (const auto& entry : fs::directory_iterator(dir)) {
        std::ifstream in{entry.path()};
        std::stringstream ss;
        ss << in.rdbuf();
        all += ss.str();
    }
    return all;
}

bool contains(const std::string& haystack, const char* needle) { return haystack.find(needle) != std::string::npos; }

LogConfig file_logging(const fs::path& dir, bool manual = false, int timeout_ms = 2000) {
    LogConfig cfg;
    cfg.backend.manual_backend = manual;
    cfg.backend.fatal_flush_timeout = std::chrono::milliseconds{timeout_ms};
    LogOptions o;
    o.to_console = false;
    o.filename = (dir / "quark.log").string();
    cfg.loggers.emplace(std::string{kQuarkLoggerName}, o);
    return cfg;
}

Logger* qlog() { return get_logger(kQuarkLoggerName); }

void handler_prints(const char* message) noexcept { std::fprintf(stderr, "handler saw: %s\n", message); }
void handler_reenters(const char*) noexcept { fatal_error("fatal inside the handler"); }

// ---- death-test children ----

[[noreturn]] void fatal_with_handler() {
    set_fatal_handler(&handler_prints);
    fatal_error("boom2");
}

[[noreturn]] void fatal_with_reentering_handler() {
    set_fatal_handler(&handler_reenters);
    fatal_error("first");
}

[[noreturn]] void fatal_with_backend_thread(const fs::path& dir) {
    init_log(file_logging(dir));
    QLOG_INFO(qlog(), "before the fatal error");
    fatal_error("disk on fire", "plant.cpp", 42);
}

[[noreturn]] void uncaught_exception(const fs::path& dir) {
    init_log(file_logging(dir));
#if defined(__cpp_exceptions)
    std::thread{[] { throw std::runtime_error("kaboom"); }}.join();
#else
    std::terminate();  // what an uncaught error amounts to without exceptions
#endif
    std::_Exit(1);
}

[[noreturn]] void crash_signal(const fs::path& dir) {
    LogConfig cfg = file_logging(dir);
    cfg.loggers.at(std::string{kQuarkLoggerName}).level = quill::LogLevel::Warning;  // as in the HFT profile
    init_log(cfg);
    struct sigaction sa{};
    ::sigaction(SIGINT, nullptr, &sa);
    if (sa.sa_handler != SIG_DFL) std::_Exit(3);  // SIGINT/SIGTERM stay the application's
    ::sigaction(SIGTERM, nullptr, &sa);
    if (sa.sa_handler != SIG_DFL) std::_Exit(4);
    QLOG_WARN(qlog(), "before the crash");
    ::raise(SIGSEGV);
    std::_Exit(5);
}

[[noreturn]] void fatal_on_the_poller_thread(const fs::path& dir) {
    init_log(file_logging(dir, true));
    LogPoller poller;
    QLOG_INFO(qlog(), "logged on the poller thread");
    fatal_error("fatal on the poller thread");
}

[[noreturn]] void fatal_on_a_worker_while_polling(const fs::path& dir) {
    init_log(file_logging(dir, true));
    LogPoller poller;
    std::thread worker{[] {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        QLOG_INFO(qlog(), "logged on a worker");
        fatal_error("fatal on a worker");
    }};
    for (;;) poller.poll_for(std::chrono::microseconds{100});
}

[[noreturn]] void fatal_while_nobody_polls(const fs::path& dir) {
    init_log(file_logging(dir, true, 200));
    LogPoller poller;  // bound to this thread, which then never polls
    std::thread{[] { fatal_error("nobody is polling"); }}.join();
    std::_Exit(1);
}

[[noreturn]] void tsc_stays_calibrated() {
    LogConfig cfg;
    cfg.loggers[std::string{kQuarkLoggerName}].to_rotating_file = false;
    cfg.loggers[std::string{kQuarkLoggerName}].to_console = false;
    init_log(cfg);
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        const int64_t diff = RdtscClock::tscns() - RdtscClock::sysns();
        if (diff > 1'000'000 || diff < -1'000'000) std::_Exit(2);  // more than 1 ms off
    }
    shutdown_log();
    std::_Exit(0);
}

#if defined(QUARK_NO_EXCEPTIONS)
// quark itself is built without exceptions: its terminate handler can't look at the exception
constexpr const char* kTerminateMessage = "fatal error: std::terminate called";
#else
constexpr const char* kTerminateMessage = "fatal error: uncaught exception: kaboom";
#endif

}  // namespace

TEST(Fatal, NamespaceIsTheMajorVersion) {
    static_assert(std::is_same_v<quark::QuarkError, quark::v1::QuarkError>);
    static_assert(QUARK_VERSION_MAJOR == 1);
}

TEST(Fatal, ReportsOnStderrThenAborts) {
    EXPECT_EXIT(fatal_error("boom", "file.cpp", 7), ::testing::KilledBySignal(SIGABRT),
                "Quark fatal error: boom \\(file\\.cpp:7\\)");
    EXPECT_EXIT(fatal_error("no location"), ::testing::KilledBySignal(SIGABRT), "Quark fatal error: no location");
}

TEST(Fatal, HandlerRunsFirstAndCanBeSwapped) {
    EXPECT_DEATH(fatal_with_handler(), "Quark fatal error: boom2.*handler saw: boom2");
    EXPECT_EQ(set_fatal_handler(&handler_prints), nullptr);
    EXPECT_EQ(set_fatal_handler(nullptr), &handler_prints);
}

TEST(Fatal, ReenteringFromTheHandlerAbortsAtOnce) {
    EXPECT_DEATH(fatal_with_reentering_handler(), "Quark fatal error: first");
}

TEST(Fatal, MessageReachesTheLogFile) {
    const fs::path dir = fresh_dir("backend_thread");
    EXPECT_DEATH(fatal_with_backend_thread(dir), "Quark fatal error: disk on fire");
    const std::string logs = read_logs(dir);
    EXPECT_TRUE(contains(logs, "before the fatal error")) << logs;
    EXPECT_TRUE(contains(logs, "fatal error: disk on fire (plant.cpp:42)")) << logs;
}

TEST(Fatal, UncaughtExceptionIsLogged) {
    const fs::path dir = fresh_dir("terminate");
    EXPECT_DEATH(uncaught_exception(dir), kTerminateMessage);
    const std::string logs = read_logs(dir);
    EXPECT_TRUE(contains(logs, kTerminateMessage)) << logs;
}

TEST(Fatal, CrashSignalIsLoggedAndSigintSigtermAreLeftAlone) {
    const fs::path dir = fresh_dir("signal");
    EXPECT_EXIT(crash_signal(dir), ::testing::KilledBySignal(SIGSEGV), "");
    const std::string logs = read_logs(dir);
    EXPECT_TRUE(contains(logs, "before the crash")) << logs;
    EXPECT_TRUE(contains(logs, "Received signal")) << logs;
}

TEST(Fatal, ManualBackendFlushesFromThePollerThread) {
    const fs::path dir = fresh_dir("manual_same_thread");
    EXPECT_DEATH(fatal_on_the_poller_thread(dir), "Quark fatal error: fatal on the poller thread");
    const std::string logs = read_logs(dir);
    EXPECT_TRUE(contains(logs, "logged on the poller thread")) << logs;
    EXPECT_TRUE(contains(logs, "fatal error: fatal on the poller thread")) << logs;
}

TEST(Fatal, ManualBackendFlushesWhileThePollerRuns) {
    const fs::path dir = fresh_dir("manual_other_thread");
    EXPECT_DEATH(fatal_on_a_worker_while_polling(dir), "Quark fatal error: fatal on a worker");
    const std::string logs = read_logs(dir);
    EXPECT_TRUE(contains(logs, "logged on a worker")) << logs;
    EXPECT_TRUE(contains(logs, "fatal error: fatal on a worker")) << logs;
}

// nobody polls the manual backend: the flush can't complete, the process must still die soon
TEST(Fatal, FlushIsBoundedByTheTimeout) {
    const fs::path dir = fresh_dir("bounded");
    const auto start = std::chrono::steady_clock::now();
    EXPECT_DEATH(fatal_while_nobody_polls(dir), "Quark fatal error: nobody is polling.*log flush timed out after 200 ms");
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{10});
}

TEST(Fatal, CrashOptionsAreReadFromTheConfig) {
    auto cfg = QuarkConfig::load_string("[log]\ncrash_handler = false\nfatal_flush_timeout_ms = 250\n");
    ASSERT_TRUE(cfg.has_value());
    const LogConfig config = LogConfig::load_from_config(*cfg);
    EXPECT_FALSE(config.backend.crash_handler);
    EXPECT_EQ(config.backend.fatal_flush_timeout, std::chrono::milliseconds{250});
    EXPECT_TRUE(LogBackendOptions{}.crash_handler);  // on by default
}

// the log backend calibrates RdtscClock: tscns() keeps tracking the system clock
TEST(Fatal, LogBackendKeepsTheTscCalibrated) {
    EXPECT_EXIT(tsc_stays_calibrated(), ::testing::ExitedWithCode(0), "");
}
