#pragma once

// quark logging, built on quill. quill has a single backend per process, so:
//   - only the process (main) calls init_log() and owns the backend, either quill's own thread or,
//     with `manual_backend`, a LogPoller on a thread of its choosing;
//   - libraries (quark itself, networking, strategies) only call get_logger(), never quill::Backend.
//
//   auto cfg = quark::QuarkConfig::load_file("app.toml");
//   quark::ScopedLog scoped_log{*cfg};                  // reads [log] and [log.<name>]
//   quark::Logger* net = quark::get_logger("net");
//   QLOG_INFO(net, "connected to {}:{}", host, port);

#ifndef QUILL_DISABLE_NON_PREFIXED_MACROS
    #define QUILL_DISABLE_NON_PREFIXED_MACROS
#endif

#include "quark/Attributes.h"

#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Frontend queue settings, fixed at build time and shared by every logger of the process:
// -DQUARK_LOG_QUEUE_TYPE=..., -DQUARK_LOG_INITIAL_QUEUE_CAPACITY=<bytes>, -DQUARK_LOG_HUGE_PAGES=ON|OFF.
#ifndef QUARK_LOG_QUEUE_TYPE
    #define QUARK_LOG_QUEUE_TYPE BoundedDropping
#endif
#ifndef QUARK_LOG_INITIAL_QUEUE_CAPACITY
    #define QUARK_LOG_INITIAL_QUEUE_CAPACITY (2u * 1024u * 1024u)
#endif
#ifndef QUARK_LOG_HUGE_PAGES
    #define QUARK_LOG_HUGE_PAGES 1
#endif

QUARK_BEGIN_NAMESPACE

class QuarkConfig;

struct FrontendOptions : quill::FrontendOptions {
    static constexpr quill::QueueType queue_type = quill::QueueType::QUARK_LOG_QUEUE_TYPE;
    static constexpr std::size_t initial_queue_capacity = QUARK_LOG_INITIAL_QUEUE_CAPACITY;
    static constexpr quill::HugePagesPolicy huge_pages_policy =
        QUARK_LOG_HUGE_PAGES ? quill::HugePagesPolicy::Try : quill::HugePagesPolicy::Never;
};

using Frontend = quill::FrontendImpl<FrontendOptions>;
using Logger = quill::LoggerImpl<FrontendOptions>;

// Name of quark's own logger, configured by the optional [log.quark] table.
inline constexpr std::string_view kQuarkLoggerName{"quark"};

// Backend settings, read from the [log] table.
struct QUARK_API LogBackendOptions {
    // Drive the backend from a LogPoller instead of quill's own thread.
    bool manual_backend{false};
    // Cores of quill's backend thread, empty means no pinning. `cpu_affinity` in [log].
    std::vector<uint16_t> cpu_affinity{};
    // Idle sleep of quill's backend thread, 0 busy-polls. `backend_sleep_ns` in [log].
    std::chrono::nanoseconds sleep_duration{std::chrono::microseconds{100}};
    // Only with sleep_duration == 0: yield the core when idle instead of spinning.
    bool yield_when_idle{false};
};

// Options of one logger, read from its [log.<name>] table. Missing keys keep these defaults.
struct QUARK_API LogOptions {
    bool to_console{true};
    bool to_rotating_file{true};

#if defined(NDEBUG)
    quill::LogLevel level{quill::LogLevel::Info};
#else
    quill::LogLevel level{quill::LogLevel::Debug};
#endif

    // quill pattern of a log line, empty uses quark's default.
    std::string pattern{};

    // Empty means "logs/<logger name>.log". Loggers sharing a filename share the file sink.
    std::string filename{};
    // 0 disables size-based rotation.
    std::int64_t max_file_size_bytes{128 * 1024 * 1024};
    // 0 keeps every rotated file.
    std::int64_t max_backup_files{10};
    // HH:MM (GMT) of the daily rotation, empty disables it.
    std::string daily_rotation_time{"00:00"};

    static LogOptions load_from_config(const QuarkConfig& cfg, std::string_view section);
};

struct QUARK_API LogConfig {
    LogBackendOptions backend;
    // Options of the loggers get_logger() creates, by name. Others get LogOptions{}.
    std::map<std::string, LogOptions, std::less<>> loggers;

    // Backend options from [<section>], one logger per [<section>.<name>] table.
    static LogConfig load_from_config(const QuarkConfig& cfg, std::string_view section = "log");
};

// Called once by the process owner, before other threads log. Creates the configured loggers and
// starts quill's backend thread, unless `manual_backend`. Returns false if already called.
// A logger created by get_logger() earlier keeps its sinks, only its level is applied.
QUARK_API bool init_log(const LogConfig& config = LogConfig{});
QUARK_API bool init_log(const QuarkConfig& cfg, std::string_view section = "log");

// Drains every queue and stops quill's backend thread. With `manual_backend` the LogPoller does it.
QUARK_API void shutdown_log() noexcept;

// Returns the logger `name`, creating it from its config (or defaults) on first use. Never null,
// thread-safe. Look a logger up once and keep the pointer. Libraries should prefix their names,
// e.g. "lepton.ws", quill loggers of other components share the same namespace.
[[nodiscard]] QUARK_API Logger* get_logger(std::string_view name);

// quark::Frontend::preallocate() creates the calling thread's queue now instead of on its first
// log statement, call it at the start of latency-sensitive threads.

// Manual backend, with init_log({.backend = {.manual_backend = true}}). Binds the backend to the
// constructing thread: polling and destruction must happen on that same thread.
//
//   quark::LogPoller poller;
//   while (running) {
//       poller.poll_for(std::chrono::microseconds{50});
//   }
class QUARK_API LogPoller {
public:
    LogPoller();   // At most one per process.
    ~LogPoller();  // Drains every queue and stops the backend.

    LogPoller(const LogPoller&) = delete;
    LogPoller& operator=(const LogPoller&) = delete;

    // Processes at most one record: lowest, most predictable cost per call.
    void poll_once();
    // Drains until empty or until `timeout` elapses.
    void poll_for(std::chrono::microseconds timeout);
};

// init_log() on construction, shutdown_log() on destruction if this instance initialized logging.
class QUARK_API ScopedLog {
public:
    explicit ScopedLog(const LogConfig& config = LogConfig{}) : owner_(init_log(config)) {}
    explicit ScopedLog(const QuarkConfig& cfg, std::string_view section = "log") : owner_(init_log(cfg, section)) {}
    ~ScopedLog() {
        if (owner_) {
            shutdown_log();
        }
    }

    ScopedLog(const ScopedLog&) = delete;
    ScopedLog& operator=(const ScopedLog&) = delete;

    [[nodiscard]] bool owner() const noexcept { return owner_; }

private:
    bool owner_;
};

namespace detail {
[[nodiscard]] QUARK_API Logger* quark_logger();
}  // namespace detail

QUARK_END_NAMESPACE

#define QLOG_DEBUG(logger, fmt_str, ...) QUILL_LOG_DEBUG(logger, fmt_str, ##__VA_ARGS__)
#define QLOG_INFO(logger, fmt_str, ...)  QUILL_LOG_INFO(logger, fmt_str, ##__VA_ARGS__)
#define QLOG_WARN(logger, fmt_str, ...)  QUILL_LOG_WARNING(logger, fmt_str, ##__VA_ARGS__)
#define QLOG_ERROR(logger, fmt_str, ...) QUILL_LOG_ERROR(logger, fmt_str, ##__VA_ARGS__)

// quark's own records, to the "quark" logger. Usable from headers.
#if defined(QUARK_DISABLE_INTERNAL_LOG)
    #define QUARK_LOG_DEBUG(fmt_str, ...) (void)0
    #define QUARK_LOG_INFO(fmt_str, ...)  (void)0
    #define QUARK_LOG_WARN(fmt_str, ...)  (void)0
    #define QUARK_LOG_ERROR(fmt_str, ...) (void)0
#else
    #define QUARK_LOG_DEBUG(fmt_str, ...) QUILL_LOG_DEBUG(::quark::detail::quark_logger(), fmt_str, ##__VA_ARGS__)
    #define QUARK_LOG_INFO(fmt_str, ...)  QUILL_LOG_INFO(::quark::detail::quark_logger(), fmt_str, ##__VA_ARGS__)
    #define QUARK_LOG_WARN(fmt_str, ...)  QUILL_LOG_WARNING(::quark::detail::quark_logger(), fmt_str, ##__VA_ARGS__)
    #define QUARK_LOG_ERROR(fmt_str, ...) QUILL_LOG_ERROR(::quark::detail::quark_logger(), fmt_str, ##__VA_ARGS__)
#endif
