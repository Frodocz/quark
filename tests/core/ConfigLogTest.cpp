// Configuration loading and logging setup: process-wide state, so these run in their own binary.
#include "quark/QuarkConfig.h"
#include "quark/QuarkError.h"
#include "quark/QuarkLog.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace quark;

TEST(ConfigLogTest, AssertIsASingleStatement) {
    int taken = 0;
    // would not compile if QUARK_ASSERT expanded to a bare block followed by ';'
    if (taken == 0)
        QUARK_ASSERT(taken == 0);
    else
        ++taken;
    EXPECT_THROW(QUARK_ASSERT(taken == 1), QuarkError);
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

TEST(ConfigLogTest, BadLevelIsAQuarkErrorNamingTheKey) {
    auto cfg = QuarkConfig::load_string("[log.net]\nlevel = \"verbose\"\n");
    ASSERT_TRUE(cfg.has_value());
    try {
        (void)LogConfig::load_from_config(*cfg);
        FAIL() << "expected QuarkError";
    } catch (const QuarkError& e) {
        EXPECT_NE(std::string_view(e.what()).find("log.net.level"), std::string_view::npos) << e.what();
    }
}

TEST(ConfigLogTest, FailedInitCanBeRetried) {
    const auto dir = std::filesystem::temp_directory_path() / "quark_config_log_test";
    std::filesystem::create_directories(dir);
    LogConfig bad;
    LogOptions opts;
    opts.to_console = false;
    opts.filename = (dir / "net.log").string();
    opts.daily_rotation_time = "25:99";  // rejected by the file sink
    bad.loggers.emplace("net", opts);
    EXPECT_ANY_THROW(init_log(bad));

    LogConfig good = bad;
    good.loggers.at("net").daily_rotation_time = "00:00";
    EXPECT_TRUE(init_log(good));
    Logger* net = get_logger("net");
    ASSERT_NE(net, nullptr);
    QUARK_LOG_INFO("logging works after a failed init_log()");
    shutdown_log();
    std::filesystem::remove_all(dir);
}
