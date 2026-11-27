// Configuration loading and logging setup: process-wide state, so these run in their own binary.
#include "quark/QuarkConfig.h"
#include "quark/QuarkError.h"
#include "quark/QuarkLog.h"

#include <gtest/gtest.h>

#include "QuarkTest.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <thread>
#include <vector>

using namespace quark;

TEST(ConfigLogTest, AssertIsASingleStatement) {
    int taken = 0;
    // would not compile if QUARK_ASSERT expanded to a bare block followed by ';'
    if (taken == 0)
        QUARK_ASSERT(taken == 0);
    else
        ++taken;
    QUARK_EXPECT_ERROR(QUARK_ASSERT(taken == 1), "Assert failed: taken == 1");
}

TEST(ConfigLogTest, LoadFailuresReportWithoutCreatingTheLogger) {
    testing::internal::CaptureStderr();
    EXPECT_FALSE(QuarkConfig::load_file("/nonexistent/quark.toml").has_value());
    EXPECT_FALSE(QuarkConfig::load_string("a = [1, ").has_value());
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("failed to load config file /nonexistent/quark.toml"), std::string::npos) << err;
    EXPECT_NE(err.find("failed to parse config string"), std::string::npos) << err;
    // nothing was logged: the quark logger (and its default log file) doesn't exist yet
    EXPECT_EQ(Frontend::get_logger(std::string{kQuarkLoggerName}), nullptr);
}

TEST(ConfigLogTest, TypedLookups) {
    auto cfg = QuarkConfig::load_string("[a]\nn = 5\nx = 1.5\ns = \"str\"\nflag = true\n");
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->get<int64_t>("a.n"), 5);
    EXPECT_EQ(cfg->get<double>("a.x"), 1.5);
    EXPECT_EQ(cfg->get<std::string>("a.s"), "str");
    EXPECT_EQ(cfg->get<bool>("a.flag"), true);
    EXPECT_FALSE(cfg->get<int64_t>("a.missing").has_value());
    EXPECT_FALSE(cfg->get<int64_t>("a.s").has_value());  // wrong type
}

// RUN-01: TOML 5 is an integer and 5.0 a float; a double reads both, as long as the integer converts exactly
TEST(ConfigLogTest, IntegerReadsAsDouble) {
    auto cfg = QuarkConfig::load_string("[a]\nn = 3\nbig = 9007199254740993\nxs = [1, 2.5]\n");
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->get<double>("a.n"), 3.0);
    EXPECT_EQ(cfg->get_or<double>("a.n", 0.005), 3.0);
    EXPECT_EQ(cfg->get<int64_t>("a.n"), 3);
    EXPECT_FALSE(cfg->get<double>("a.big").has_value());  // 2^53 + 1: not exactly a double
    EXPECT_EQ((*cfg)["a.xs"][0].get<double>(), 1.0);
    EXPECT_EQ((*cfg)["a.xs"][0].get_or<double>(0.0), 1.0);
    EXPECT_EQ((*cfg)["a.xs"][1].get<double>(), 2.5);
    EXPECT_FALSE(cfg->get<int64_t>("a.xs[1]").has_value());  // never float -> integer
}

// RUN-01: a key that is there with another type isn't silently replaced by the default
TEST(ConfigLogTest, GetOrWarnsOnAWrongType) {
    auto cfg = QuarkConfig::load_string("[ts]\ncpu = \"3\"\ncancel_all = \"false\"\nfee = 0.5\n");
    ASSERT_TRUE(cfg.has_value());
    testing::internal::CaptureStderr();
    EXPECT_EQ(cfg->get_or<int64_t>("ts.cpu", -1), -1);
    EXPECT_EQ(cfg->get_or<bool>("ts.cancel_all", true), true);
    EXPECT_EQ(cfg->get_or<int64_t>("ts.fee", 7), 7);
    EXPECT_EQ(cfg->get_or<int64_t>("ts.missing", 9), 9);  // missing: the default, quietly
    EXPECT_FALSE(cfg->get<int64_t>("ts.cpu").has_value());  // get() stays quiet: callers probe types with it
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("config key ts.cpu is a string, expected an integer"), std::string::npos) << err;
    EXPECT_NE(err.find("config key ts.cancel_all is a string, expected a boolean"), std::string::npos) << err;
    EXPECT_NE(err.find("config key ts.fee is a float, expected an integer"), std::string::npos) << err;
    EXPECT_EQ(err.find("ts.missing"), std::string::npos) << err;
    EXPECT_EQ(std::count(err.begin(), err.end(), '\n'), 3) << err;
}

TEST(ConfigLogTest, ArraysOfTables) {
    auto cfg = QuarkConfig::load_string(R"(
[[venue]]
name = "a"
ports = [1, 2]
[venue.limits]
rate = 10

[[venue]]
name = "b"
)");
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->array_size("venue"), 2u);
    EXPECT_EQ(cfg->get<std::string>("venue[1].name"), "b");
    EXPECT_EQ(cfg->get<int64_t>("venue[0].limits.rate"), 10);
    const QuarkConfigValue first = (*cfg)["venue"][0];
    EXPECT_TRUE(first.is<QuarkConfigTable>());
    EXPECT_EQ(first.size(), 3u);
    EXPECT_EQ(first["name"].get<std::string>(), "a");
    EXPECT_EQ(first["ports"][1].get<int64_t>(), 2);
    EXPECT_EQ(first["limits"]["rate"].get<int64_t>(), 10);
    EXPECT_FALSE(first["missing"].has_value());
    EXPECT_FALSE((*cfg)["venue"][2].has_value());
    EXPECT_EQ(cfg->array_size("venue[0].name"), 0u);  // not an array
}

TEST(ConfigLogTest, BadLevelIsAQuarkErrorNamingTheKey) {
    auto cfg = QuarkConfig::load_string("[log.net]\nlevel = \"verbose\"\n");
    ASSERT_TRUE(cfg.has_value());
    QUARK_EXPECT_ERROR((void)LogConfig::load_from_config(*cfg), "log\\.net\\.level: invalid log level 'verbose'");
}

// RUN-05: quill refuses Backtrace as a logger level (QuillError at init_log), so quark doesn't accept it either
TEST(ConfigLogTest, BacktraceIsNotALoggerLevel) {
    auto cfg = QuarkConfig::load_string("[log.net]\nlevel = \"backtrace\"\n");
    ASSERT_TRUE(cfg.has_value());
    QUARK_EXPECT_ERROR((void)LogConfig::load_from_config(*cfg), "log\\.net\\.level: invalid log level 'backtrace'");
}

// RUN-07: a negative sleep would busy-poll a core, a negative flush timeout skip the flush of the fatal line
TEST(ConfigLogTest, NegativeBackendTimesAreRefused) {
    auto sleep = QuarkConfig::load_string("[log]\nbackend_sleep_ns = -1\n");
    ASSERT_TRUE(sleep.has_value());
    QUARK_EXPECT_ERROR((void)LogConfig::load_from_config(*sleep), "log\\.backend_sleep_ns must be >= 0, got -1");
    auto timeout = QuarkConfig::load_string("[log]\nfatal_flush_timeout_ms = -5\n");
    ASSERT_TRUE(timeout.has_value());
    QUARK_EXPECT_ERROR((void)LogConfig::load_from_config(*timeout), "log\\.fatal_flush_timeout_ms must be >= 0, got -5");
    auto zero = QuarkConfig::load_string("[log]\nbackend_sleep_ns = 0\nfatal_flush_timeout_ms = 0\n");
    ASSERT_TRUE(zero.has_value());
    const LogConfig config = LogConfig::load_from_config(*zero);
    EXPECT_EQ(config.backend.sleep_duration, std::chrono::nanoseconds{0});
    EXPECT_EQ(config.backend.fatal_flush_timeout, std::chrono::milliseconds{0});
}

// a negative file limit would silently mean "no rotation" / "keep every file" (0 says that explicitly)
TEST(ConfigLogTest, NegativeFileLimitsAreRefused) {
    auto size = QuarkConfig::load_string("[log.net]\nmax_file_size_bytes = -1\n");
    ASSERT_TRUE(size.has_value());
    QUARK_EXPECT_ERROR((void)LogConfig::load_from_config(*size), "log\\.net\\.max_file_size_bytes must be >= 0, got -1");
    auto backups = QuarkConfig::load_string("[log.net]\nmax_backup_files = -3\n");
    ASSERT_TRUE(backups.has_value());
    QUARK_EXPECT_ERROR((void)LogConfig::load_from_config(*backups), "log\\.net\\.max_backup_files must be >= 0, got -3");
    auto zero = QuarkConfig::load_string("[log.net]\nmax_file_size_bytes = 0\nmax_backup_files = 0\n");
    ASSERT_TRUE(zero.has_value());
    const LogOptions net = LogConfig::load_from_config(*zero).loggers.at("net");
    EXPECT_EQ(net.max_file_size_bytes, 0);
    EXPECT_EQ(net.max_backup_files, 0);
}

// RUN-01: [log] keys of the wrong type keep their default and say so
TEST(ConfigLogTest, LogKeysOfTheWrongTypeWarn) {
    auto cfg = QuarkConfig::load_string("[log]\ncrash_handler = \"no\"\n[log.net]\nto_console = 0\n");
    ASSERT_TRUE(cfg.has_value());
    testing::internal::CaptureStderr();
    const LogConfig config = LogConfig::load_from_config(*cfg);
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(config.backend.crash_handler);
    EXPECT_TRUE(config.loggers.at("net").to_console);
    EXPECT_NE(err.find("config key log.crash_handler is a string, expected a boolean"), std::string::npos) << err;
    EXPECT_NE(err.find("config key log.net.to_console is an integer, expected a boolean"), std::string::npos) << err;
}

#if !defined(QUARK_NO_EXCEPTIONS)  // the failed init_log() is fatal without exceptions: nothing to retry
namespace {
// RUN-06, in a child: quill hands out its manual backend worker once per process
[[noreturn]] void manual_init_retried() {
    LogConfig bad;
    bad.backend.manual_backend = true;
    LogOptions opts;
    opts.to_console = false;
    opts.to_rotating_file = false;
    opts.level = quill::LogLevel::Backtrace;  // refused by quill when the logger is created
    bad.loggers.emplace("net", opts);
    QUARK_TRY {
        (void)init_log(bad);
        std::_Exit(2);  // the bad configuration was accepted
    } QUARK_CATCH_ALL() {}
    LogConfig good = bad;
    good.loggers.at("net").level = quill::LogLevel::Info;
    if (!init_log(good)) std::_Exit(3);
    {
        LogPoller poller;
        QLOG_INFO(get_logger("net"), "logging works after a failed manual init_log()");
        poller.poll_for(std::chrono::microseconds{1000});
    }
    std::_Exit(0);
}
}  // namespace
#endif

namespace {
// RUN-04, in a child: a burst far beyond one thread's queue (BoundedDropping) is counted
[[noreturn]] void burst_is_counted() {
    LogConfig cfg;
    LogOptions opts;
    opts.to_console = false;
    opts.to_rotating_file = false;  // the null sink: the backend still has to format every record
    cfg.loggers.emplace("burst", opts);
    if (!init_log(cfg)) std::_Exit(2);
    if (dropped_log_records() != 0) std::_Exit(3);
    Logger* burst = get_logger("burst");
    const std::string payload(200, 'x');
    for (int i = 0; i < 500'000; ++i) QLOG_INFO(burst, "burst {} {}", i, payload);
    burst->flush_log();
    // the backend reports a thread's drops when it next reads that thread's queue
    for (int i = 0; i < 200 && dropped_log_records() == 0; ++i) {
        QLOG_INFO(burst, "after the burst");
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    const uint64_t dropped = dropped_log_records();
    std::fprintf(stderr, "counted %llu dropped records\n", static_cast<unsigned long long>(dropped));
    shutdown_log();
    std::_Exit(dropped > 0 && dropped < 500'000 ? 0 : 4);
}
}  // namespace

TEST(ConfigLogTest, DroppedRecordsAreCounted) {
    EXPECT_EXIT(burst_is_counted(), ::testing::ExitedWithCode(0), "Dropped [0-9]+ events.*counted [0-9]+ dropped records");
}

#if !defined(QUARK_NO_EXCEPTIONS)
TEST(ConfigLogTest, FailedManualInitCanBeRetried) {
    EXPECT_EXIT(manual_init_retried(), ::testing::ExitedWithCode(0), "");
}
#endif

TEST(ConfigLogTest, FailedInitCanBeRetried) {
    const auto dir = std::filesystem::temp_directory_path() / "quark_config_log_test";
    std::filesystem::remove_all(dir);  // leftovers of a crashed run
    std::filesystem::create_directories(dir);
    LogConfig bad;
    LogOptions opts;
    opts.to_console = false;
    opts.filename = (dir / "net.log").string();
    opts.daily_rotation_time = "25:99";  // rejected by the file sink
    bad.loggers.emplace("net", opts);
    QUARK_EXPECT_ANY_ERROR(init_log(bad));  // quill rejects the rotation time

    LogConfig good = bad;
    good.loggers.at("net").daily_rotation_time = "00:00";
    EXPECT_TRUE(init_log(good));
    Logger* net = get_logger("net");
    ASSERT_NE(net, nullptr);
    QUARK_LOG_INFO("logging works after a failed init_log()");
    QLOG_INFO(net, "net logs to its file");
    for (int i = 0; i < 10; ++i) QLOG_WARN_LIMIT(std::chrono::seconds(10), net, "throttled {}", i);
    shutdown_log();
    // the file name carries the process start time: net.log -> net_YYYYMMDD_HHMMSS.log
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) files.push_back(entry.path().filename().string());
    ASSERT_EQ(files.size(), 1u);
    EXPECT_TRUE(std::regex_match(files[0], std::regex{R"(net_\d{8}_\d{6}\.log)"})) << files[0];
    std::ifstream in(dir / files[0]);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    EXPECT_NE(text.find("net logs to its file"), std::string::npos) << text;
    EXPECT_NE(text.find("throttled 0"), std::string::npos) << text;
    EXPECT_EQ(text.find("throttled 1"), std::string::npos) << text;  // within the interval: suppressed
    std::filesystem::remove_all(dir);
}
