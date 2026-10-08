#pragma once

// quark logging, built on the embedded quill.
//
// Every logger is declared up front in the application's TOML config and created by init_log():
//
//   [log]                      # global: frontend queue type and backend thread affinity
//   queue_type = "BoundedDropping"
//   cpu_affinity = [3]
//
//   [log.app]                  # one table per logger, named by the user
//   level = "Info"
//
//   [log.net]
//   level = "Debug"
//   filename = "logs/net.log"
//
//   [log.quark]                # optional: quark's own logger, defaults if absent
//   level = "Warn"
//
//   auto cfg = quark::QuarkConfig::load_file("app.toml");   // the whole application config
//   quark::ScopedLog scoped_log{*cfg};                       // reads its [log] tables
//   quark::Logger net = quark::get_logger("net");
//   QLOG_INFO(net, "connected to {}:{}", host, port);
//
// After init_log() the set of loggers is fixed; only their levels can change at runtime. quark's
// internal records go to the "quark" logger, tagged with their component, e.g. "[quark.config]".
// Before init_log() and after shutdown_log(), quark's own warnings and errors go to stderr.

#ifndef QUILL_DISABLE_NON_PREFIXED_MACROS
    #define QUILL_DISABLE_NON_PREFIXED_MACROS
#endif

#include "quark/Attributes.h"

#include "quill/LogMacros.h"
#include "quill/Logger.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

QUARK_BEGIN_NAMESPACE

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------
class QuarkConfig;

// quark's own log-level enum so users never need to name quill:: types.
enum class LogLevel : uint8_t {
    Debug,
    Info,
    Warn,
    Error,
    Off,  // Only valid as a threshold: disables the logger.
};

constexpr LogLevel parse_log_level(std::string_view str) {
    if (str == "DEBUG" || str == "debug" || str == "Debug") return LogLevel::Debug;
    if (str == "INFO" || str == "Info" || str == "info") return LogLevel::Info;
    if (str == "WARN" || str == "Warn" || str == "warn") return LogLevel::Warn;
    if (str == "ERROR" || str == "Error" || str == "error") return LogLevel::Error;
    if (str == "OFF" || str == "Off" || str == "off") return LogLevel::Off;
    return LogLevel::Info;
}

enum class QueueType : uint8_t {
    UnboundedBlocking, // Small initial capacity and reallocates up to unbounded_queue_max_capacity (2 GiB) then block
    UnboundedDropping, // Small initial capacity and reallocates up to unbounded_queue_max_capacity (2 GiB) then drop
    BoundedBlocking,   // Fixed initial capacity and never reallocates, blocks the calling thread until space is available
    BoundedDropping,   // Fixed initial capacity and never reallocates, discards log messages when the limit is reached.
};

constexpr QueueType parse_queue_type(std::string_view str) {
    if (str == "UnboundedBlocking") return QueueType::UnboundedBlocking;
    if (str == "UnboundedDropping") return QueueType::UnboundedDropping;
    if (str == "BoundedBlocking") return QueueType::BoundedBlocking;
    if (str == "BoundedDropping") return QueueType::BoundedDropping;
    return QueueType::BoundedDropping;
}

// Name of quark's own logger, configured by the optional [log.quark] table.
inline constexpr std::string_view kQuarkLoggerName{"quark"};

// Capacity in bytes of each logging thread's queue: the fixed size of a bounded queue, the starting
// size of an unbounded one. Shared by all loggers used on that thread. quill only accepts it at
// compile time, so it is a build option (-DQUARK_LOG_INITIAL_QUEUE_CAPACITY=...), not a config key.
#ifndef QUARK_LOG_INITIAL_QUEUE_CAPACITY
    #define QUARK_LOG_INITIAL_QUEUE_CAPACITY (2u * 1024u * 1024u)
#endif
inline constexpr std::size_t kLogInitialQueueCapacity{QUARK_LOG_INITIAL_QUEUE_CAPACITY};
static_assert(kLogInitialQueueCapacity > 0, "QUARK_LOG_INITIAL_QUEUE_CAPACITY must be a positive number of bytes");

// Back those queues with 2 MiB huge pages to cut TLB misses on the logging threads, falling back to
// normal pages when the machine has none available (see vm.nr_hugepages). Build option
// -DQUARK_LOG_HUGE_PAGES=ON|OFF.
#ifndef QUARK_LOG_HUGE_PAGES
    #define QUARK_LOG_HUGE_PAGES 1
#endif
inline constexpr bool kLogHugePages{QUARK_LOG_HUGE_PAGES != 0};

// Process-wide settings, read from the top-level [log] table. quill fixes them for the whole
// process, so they cannot be set per logger.
struct QUARK_API LogGlobalOptions {
    // --- Queue type and memory allocation policies (frontend option) ---
    QueueType queue_type{QueueType::BoundedDropping};

    // --- Set CPU affinity for the backend thread (backend option) ---
    // Defaults to core 0; set it to the core reserved for logging. Empty means no pinning. If the core
    // is not available the backend still runs, unpinned, and quill prints a warning.
    std::vector<int64_t> cpu_affinity{0};

    // --- Backend polling (backend option) ---
    // How long the backend thread sleeps once every queue is empty, `backend_sleep_ns` in [log].
    // Defaults to 0, busy-polling: lowest latency from log call to file, but the thread spins at 100%
    // of its core, hence the pinning above.
    std::chrono::nanoseconds backend_sleep_duration{0};
    // Only with backend_sleep_duration == 0: yield the core when idle instead of spinning.
    bool backend_yield_when_idle{false};
};

// Options of one logger, read from its [log.<name>] table. Missing keys keep these defaults.
// Only `level` can be changed after init_log(), via Logger::set_level().
struct QUARK_API LogOptions {
    // --- Sink selection (either RotatingFile or ConsoleSink, or both) ---
    bool to_console{true};
    bool to_rotating_file{true};

    // Defaults to Debug in Debug builds, Info in optimized (NDEBUG) builds.
#if defined(NDEBUG)
    LogLevel level{LogLevel::Info};
#else
    LogLevel level{LogLevel::Debug};
#endif

    // --- Formatting ---
    // Quill pattern for the log line. Empty uses quark's default pattern.
    std::string pattern{};

    // --- RotatingFile options at daily basis ---
    // Base filename, empty means "logs/<logger name>.log". Rotated files derive from this
    // (e.g. app.1.log, ...). Loggers that share a filename share the same file sink (the first
    // config wins).
    std::string filename{};
    // Roll to a new file once the current one reaches this many bytes. 0 disables size-based rotation.
    std::int64_t max_file_size_bytes{128 * 1024 * 1024};  // 128 MiB
    // Keep at most this many rotated files; older ones are removed. Bounds disk
    // usage for an infinite-loop process. 0 means "no count limit".
    std::int64_t max_backup_files{10};
    // Also roll daily at this HH:MM (GmtTime). Empty disables time-based rotation.
    std::string daily_rotation_time{"00:00"};

    // Reads `<section>.<field>` keys, e.g. `log.app.level`.
    static LogOptions load_from_config(const QuarkConfig& cfg, std::string_view section);
};

// Everything init_log() needs, loaded from the application's config or filled in code.
struct QUARK_API LogConfig {
    LogGlobalOptions global;
    // Logger name -> options. kQuarkLoggerName configures quark's own logger.
    std::map<std::string, LogOptions, std::less<>> loggers;

    // Global options from [<section>], one logger per [<section>.<name>] table.
    static LogConfig load_from_config(const QuarkConfig& cfg, std::string_view section = "log");
};

// -----------------------------------------------------------------------------
// Logger
// -----------------------------------------------------------------------------
namespace detail {

constexpr quill::QueueType to_quill_queue_type(QueueType type) noexcept {
    switch (type) {
        case QueueType::UnboundedBlocking: return quill::QueueType::UnboundedBlocking;
        case QueueType::UnboundedDropping: return quill::QueueType::UnboundedDropping;
        case QueueType::BoundedBlocking:   return quill::QueueType::BoundedBlocking;
        case QueueType::BoundedDropping:   return quill::QueueType::BoundedDropping;
    }
    return quill::QueueType::BoundedDropping;
}

// Quill selects the frontend queue at compile time, one FrontendOptions type per QueueType.
template <QueueType Type>
struct FrontendOptions : quill::FrontendOptions {
    static constexpr quill::QueueType queue_type = to_quill_queue_type(Type);
    static constexpr std::size_t initial_queue_capacity = kLogInitialQueueCapacity;
    static constexpr quill::HugePagesPolicy huge_pages_policy =
        kLogHugePages ? quill::HugePagesPolicy::Try : quill::HugePagesPolicy::Never;
};

template <QueueType Type>
using LoggerImpl = quill::LoggerImpl<FrontendOptions<Type>>;

// Calls f(std::integral_constant<QueueType, type>{}) so f can name the matching compile-time types.
template <typename F>
decltype(auto) visit_queue_type(QueueType type, F&& f) {
    using enum QueueType;
    switch (type) {
        case UnboundedBlocking: return f(std::integral_constant<QueueType, UnboundedBlocking>{});
        case UnboundedDropping: return f(std::integral_constant<QueueType, UnboundedDropping>{});
        case BoundedBlocking:   return f(std::integral_constant<QueueType, BoundedBlocking>{});
        case BoundedDropping:
        default:                return f(std::integral_constant<QueueType, BoundedDropping>{});
    }
}

struct LoggerFactory;

}  // namespace detail

// Lightweight, copyable handle to a logger created by init_log(). Obtain one with get_logger(),
// keep it, and log with QLOG_*(logger, fmt, ...). A default constructed handle is invalid and
// silently discards log statements.
class QUARK_API Logger {
public:
    Logger() = default;

    [[nodiscard]] bool valid() const noexcept { return logger_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] LogLevel level() const noexcept;
    void set_level(LogLevel level) noexcept; // Thread-safe, takes effect immediately.

    // Blocks until the backend has written every message logged so far by this thread.
    void flush() const;

    // --- Interface used by the QUILL_LOG_* macros, which QLOG_* expand to ---
    const Logger* operator->() const noexcept { return this; }

    template <quill::LogLevel Level>
    [[nodiscard]] bool should_log_statement() const noexcept {
        return logger_ && logger_->template should_log_statement<Level>();
    }

    template <bool ImmediateFlush, typename... Args>
    bool log_statement(quill::MacroMetadata const* metadata, Args&&... args) const {
        return detail::visit_queue_type(queue_type_, [&](auto type) {
            return static_cast<detail::LoggerImpl<decltype(type)::value>*>(logger_)
                ->template log_statement<ImmediateFlush>(metadata, static_cast<Args&&>(args)...);
        });
    }

private:
    friend struct detail::LoggerFactory;

    Logger(quill::detail::LoggerBase* logger, QueueType queue_type) noexcept
        : logger_(logger), queue_type_(queue_type) {}

    quill::detail::LoggerBase* logger_{nullptr};
    QueueType queue_type_{QueueType::BoundedDropping};
};

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------
// Starts the quill backend (global.cpu_affinity), fixes the frontend queue type
// (global.queue_type) and creates quark's own logger plus every logger in `config.loggers`.
// Returns `false` and ignores `config` if logging was already initialized.
// Throws on invalid options or when a sink cannot be created.
QUARK_API bool init_log(const LogConfig& config = LogConfig{});

// Same, reading the LogConfig from the [<section>] tables of the application's full config.
QUARK_API bool init_log(const QuarkConfig& cfg, std::string_view section = "log");

// Creates the calling thread's log queue now instead of on its first log statement, which would
// otherwise pay for allocating and pre-faulting kLogInitialQueueCapacity bytes. Call it at the start
// of every latency-sensitive thread, after init_log(). No-op if logging is not running.
QUARK_API void preallocate_log() noexcept;

// Flushes and stops the backend.
// Final: get_logger() returns invalid loggers afterwards.
// Stop logging through QLOG_* in other threads before calling it.
QUARK_API void shutdown_log() noexcept;

// Returns the logger created by init_log() for `name`, or an invalid Logger if there is none.
// Lock-free: the set of loggers never changes after init_log().
// Look loggers up once and keep the handle rather than calling this per log statement.
[[nodiscard]] QUARK_API Logger get_logger(std::string_view name) noexcept;

// RAII scope for the logging system: init_log() on construction, shutdown_log() on destruction
// (only if this instance performed the initialization).
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
    ScopedLog(ScopedLog&&) = delete;
    ScopedLog& operator=(ScopedLog&&) = delete;

    [[nodiscard]] bool owner() const noexcept { return owner_; }

private:
    bool owner_;
};

QUARK_END_NAMESPACE

// -----------------------------------------------------------------------------
// Logging Macros
// -----------------------------------------------------------------------------
// fmt_str must be a string literal using {} placeholders.
// logger: a quark::Logger from get_logger() (or a pointer to one).
#define QLOG_DEBUG(logger, fmt_str, ...) QUILL_LOG_DEBUG(logger, fmt_str, ##__VA_ARGS__)
#define QLOG_INFO(logger, fmt_str, ...)  QUILL_LOG_INFO(logger, fmt_str, ##__VA_ARGS__)
#define QLOG_WARN(logger, fmt_str, ...)  QUILL_LOG_WARNING(logger, fmt_str, ##__VA_ARGS__)
#define QLOG_ERROR(logger, fmt_str, ...) QUILL_LOG_ERROR(logger, fmt_str, ##__VA_ARGS__)
