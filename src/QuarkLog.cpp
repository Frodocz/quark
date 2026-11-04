#include "quark/QuarkLog.h"

#include "quark/QuarkConfig.h"
#include "quark/QuarkError.h"
#include "quark/utils/RdtscClock.h"

#include "quill/Backend.h"
#include "quill/sinks/ConsoleSink.h"
#include "quill/sinks/NullSink.h"
#include "quill/sinks/RotatingFileSink.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

static_assert(quill::VersionMajor == QUARK_QUILL_VERSION_MAJOR && quill::VersionMinor == QUARK_QUILL_VERSION_MINOR &&
                  quill::VersionPatch == QUARK_QUILL_VERSION_PATCH,
              "quark and every module of the process must use the same quill version, see QUARK_QUILL_VERSION");

QUARK_BEGIN_NAMESPACE

namespace {

constexpr const char* kDefaultPattern{
    "%(time) [%(thread_id)] %(short_source_location:<20) %(log_level:<8) %(logger:<6) %(message)"};
constexpr const char* kDefaultTimestampPattern{"%Y-%m-%d %H:%M:%S.%Qns"};
constexpr const char* kConsoleSinkName{"quark_console"};
constexpr const char* kNullSinkName{"quark_null"};
constexpr const char* kBackendThreadName{"QuarkLogBackend"};
// quill's crash handler logs at Info; this clone of quark's logger (same sinks) shows Info even
// when [log.quark] is set to Warn, so the crash line isn't filtered out
constexpr const char* kCrashLoggerName{"quark.crash"};

struct LogContext {
    std::mutex mtx;
    bool initialized{false};
    LogConfig config;
    quill::ManualBackendWorker* manual_worker{nullptr};
    bool poller_created{false};
};

// Leaked on purpose so loggers stay usable from static destructors.
LogContext& context() {
    static auto* ctx = new LogContext;
    return *ctx;
}

// Read by fatal_error() without taking LogContext::mtx (the failing thread may hold it).
enum class BackendMode : int { None, Thread, Manual };
std::atomic<BackendMode> g_backend_mode{BackendMode::None};
std::atomic<pid_t> g_poller_tid{0};  // the thread bound to the manual backend
std::atomic<int64_t> g_fatal_flush_timeout_ms{2000};
std::atomic<FatalHandler> g_fatal_handler{nullptr};
std::atomic<bool> g_in_fatal{false};

pid_t current_tid() noexcept { return static_cast<pid_t>(::syscall(SYS_gettid)); }

// quill::loglevel_from_string() without exceptions: same names, case-insensitive.
std::optional<quill::LogLevel> parse_log_level(std::string_view name) noexcept {
    struct Entry {
        std::string_view name;
        quill::LogLevel level;
    };
    static constexpr Entry kLevels[] = {
        {"tracel3", quill::LogLevel::TraceL3}, {"trace_l3", quill::LogLevel::TraceL3},
        {"tracel2", quill::LogLevel::TraceL2}, {"trace_l2", quill::LogLevel::TraceL2},
        {"tracel1", quill::LogLevel::TraceL1}, {"trace_l1", quill::LogLevel::TraceL1},
        {"debug", quill::LogLevel::Debug},     {"info", quill::LogLevel::Info},
        {"notice", quill::LogLevel::Notice},   {"warning", quill::LogLevel::Warning},
        {"warn", quill::LogLevel::Warning},    {"error", quill::LogLevel::Error},
        {"err", quill::LogLevel::Error},       {"critical", quill::LogLevel::Critical},
        {"backtrace", quill::LogLevel::Backtrace}, {"none", quill::LogLevel::None},
    };
    for (const auto& e : kLevels) {
        if (e.name.size() != name.size()) continue;
        bool equal = true;
        for (std::size_t i = 0; i < name.size() && equal; ++i) {
            equal = std::tolower(static_cast<unsigned char>(name[i])) == e.name[i];
        }
        if (equal) return e.level;
    }
    return std::nullopt;
}

// Logs the fatal message, then waits (bounded) for the backend to write it out.
void flush_logs_on_fatal(const char* message, const char* file, int line) noexcept {
    const BackendMode mode = g_backend_mode.load();
    if (mode == BackendMode::None) return;
    Logger* logger = Frontend::get_logger(std::string{kQuarkLoggerName});
    if (logger == nullptr) logger = Frontend::get_valid_logger();
    if (logger == nullptr) return;
    if (file != nullptr) {
        QUILL_LOG_CRITICAL(logger, "fatal error: {} ({}:{})", message, file, line);
    } else {
        QUILL_LOG_CRITICAL(logger, "fatal error: {}", message);
    }

    const auto timeout = std::chrono::milliseconds{g_fatal_flush_timeout_ms.load()};
    if (mode == BackendMode::Manual && current_tid() == g_poller_tid.load()) {
        // the manual backend is bound to this very thread: drive it from here
        context().manual_worker->poll(std::chrono::duration_cast<std::chrono::microseconds>(timeout));
        return;
    }
    // flush_log() spins until the backend answers: run it aside and give up after the timeout
    static std::atomic<bool> flushed{false};
    std::thread flusher{[logger] {
        logger->flush_log();
        flushed.store(true);
    }};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!flushed.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    if (flushed.load()) {
        flusher.join();
    } else {
        flusher.detach();  // dies with the process
        std::fprintf(stderr, "Quark fatal error: log flush timed out after %lld ms\n",
                     static_cast<long long>(timeout.count()));
    }
}

[[noreturn]] void on_terminate() noexcept {
#if !defined(QUARK_NO_EXCEPTIONS)
    if (const std::exception_ptr ep = std::current_exception()) {
        char msg[512];
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            std::snprintf(msg, sizeof(msg), "uncaught exception: %s", e.what());
        } catch (...) {
            std::snprintf(msg, sizeof(msg), "uncaught exception of a non-std::exception type");
        }
        fatal_error(msg);
    }
#endif
    fatal_error("std::terminate called");
}

// quill backend options shared by both modes: the backend thread (or the LogPoller's thread)
// keeps RdtscClock calibrated, a single caller as tscns requires
void set_common_backend_options(quill::BackendOptions& options) {
    options.backend_worker_on_poll_end = [] { RdtscClock::calibrate(); };
}

std::vector<std::shared_ptr<quill::Sink>> make_sinks(const std::string& name, const LogOptions& options) {
    std::vector<std::shared_ptr<quill::Sink>> sinks;

    if (options.to_console) {
        sinks.push_back(Frontend::create_or_get_sink<quill::ConsoleSink>(kConsoleSinkName));
    }

    if (options.to_rotating_file) {
        quill::RotatingFileSinkConfig cfg;
        cfg.set_open_mode('a');
        cfg.set_timezone(quill::Timezone::GmtTime);
        cfg.set_filename_append_option(quill::FilenameAppendOption::StartDateTime);
        if (options.max_file_size_bytes > 0) {
            cfg.set_rotation_max_file_size(static_cast<size_t>(options.max_file_size_bytes));
        }
        if (options.max_backup_files > 0) {
            cfg.set_max_backup_files(static_cast<uint32_t>(
                std::min<int64_t>(options.max_backup_files, std::numeric_limits<uint32_t>::max())));
        }
        if (!options.daily_rotation_time.empty()) {
            cfg.set_rotation_time_daily(options.daily_rotation_time);
        }
        const std::string filename = options.filename.empty() ? "logs/" + name + ".log" : options.filename;
        sinks.push_back(Frontend::create_or_get_sink<quill::RotatingFileSink>(filename, cfg));
    }

    if (sinks.empty()) {
        sinks.push_back(Frontend::create_or_get_sink<quill::NullSink>(kNullSinkName));
    }
    return sinks;
}

// Caller holds ctx.mtx.
Logger* create_logger_locked(LogContext& ctx, const std::string& name) {
    if (Logger* logger = Frontend::get_logger(name)) {
        return logger;
    }
    const auto it = ctx.config.loggers.find(name);
    const LogOptions options = it != ctx.config.loggers.end() ? it->second : LogOptions{};

    const quill::PatternFormatterOptions pattern{
        options.pattern.empty() ? std::string{kDefaultPattern} : options.pattern, kDefaultTimestampPattern};
    Logger* logger = Frontend::create_or_get_logger(name, make_sinks(name, options), pattern);
    logger->set_log_level(options.level);
    return logger;
}

template <typename T>
void assign(const QuarkConfig& cfg, const std::string& path, T& out) {
    if (auto value = cfg.get<T>(path)) {
        out = std::move(*value);
    }
}

std::string join_path(std::string_view section, std::string_view field) {
    std::string path{section};
    path += '.';
    path += field;
    return path;
}

}  // namespace

// -----------------------------------------------------------------------------
// Config
// -----------------------------------------------------------------------------
LogOptions LogOptions::load_from_config(const QuarkConfig& cfg, std::string_view section) {
    const auto key = [section](std::string_view field) { return join_path(section, field); };

    LogOptions options;
    assign(cfg, key("to_console"), options.to_console);
    assign(cfg, key("to_rotating_file"), options.to_rotating_file);
    if (auto level = cfg.get<std::string>(key("level"))) {
        const auto parsed = parse_log_level(*level);
        if (!parsed) {
            QUARK_THROW(QuarkError{key("level") + ": invalid log level '" + *level +
                                   "', expected one of tracel3, tracel2, tracel1, debug, info, notice, warning, error, "
                                   "critical, backtrace, none"});
        }
        options.level = *parsed;
    }
    assign(cfg, key("pattern"), options.pattern);
    assign(cfg, key("filename"), options.filename);
    assign(cfg, key("max_file_size_bytes"), options.max_file_size_bytes);
    assign(cfg, key("max_backup_files"), options.max_backup_files);
    assign(cfg, key("daily_rotation_time"), options.daily_rotation_time);
    return options;
}

LogConfig LogConfig::load_from_config(const QuarkConfig& cfg, std::string_view section) {
    const auto key = [section](std::string_view field) { return join_path(section, field); };

    LogConfig config;
    assign(cfg, key("manual_backend"), config.backend.manual_backend);
    if (auto cpus = cfg.get<QuarkConfigArray>(key("cpu_affinity"))) {
        for (const auto& cpu : *cpus) {
            const auto id = cpu.get<int64_t>();
            if (!id || *id < 0 || *id > std::numeric_limits<uint16_t>::max()) {
                QUARK_THROW(QuarkError{key("cpu_affinity") + " must contain cpu ids"});
            }
            config.backend.cpu_affinity.push_back(static_cast<uint16_t>(*id));
        }
    }
    if (auto sleep_ns = cfg.get<int64_t>(key("backend_sleep_ns"))) {
        config.backend.sleep_duration = std::chrono::nanoseconds{std::max<int64_t>(*sleep_ns, 0)};
    }
    assign(cfg, key("backend_yield_when_idle"), config.backend.yield_when_idle);
    assign(cfg, key("crash_handler"), config.backend.crash_handler);
    if (auto timeout_ms = cfg.get<int64_t>(key("fatal_flush_timeout_ms"))) {
        config.backend.fatal_flush_timeout = std::chrono::milliseconds{std::max<int64_t>(*timeout_ms, 0)};
    }

    for (std::string& name : cfg.table_names(section)) {
        LogOptions options = LogOptions::load_from_config(cfg, join_path(section, name));
        config.loggers.emplace(std::move(name), std::move(options));
    }
    return config;
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------
bool init_log(const LogConfig& config) {
    auto& ctx = context();
    bool backend_taken = false;
    std::vector<std::string> created_early;
    {
        std::lock_guard lock{ctx.mtx};
        if (ctx.initialized) {
            return false;
        }

        backend_taken = quill::Backend::is_running();
        if (config.backend.manual_backend) {
            if (backend_taken) {
                QUARK_THROW(QuarkError{"init_log(): manual_backend, but the quill backend is already running"});
            }
            ctx.manual_worker = quill::Backend::acquire_manual_backend_worker();
        } else {
            quill::BackendOptions options;
            options.thread_name = kBackendThreadName;
            options.cpu_affinity = config.backend.cpu_affinity;
            options.sleep_duration = config.backend.sleep_duration;
            options.enable_yield_when_idle = config.backend.yield_when_idle;
            set_common_backend_options(options);
            if (config.backend.crash_handler) {
                // crash signals only: SIGINT/SIGTERM stay the application's (quill would _Exit on them)
                quill::SignalHandlerOptions signals;
                signals.catchable_signals = {SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV};
                signals.logger_name = kCrashLoggerName;
                signals.timeout_seconds = static_cast<uint32_t>(
                    std::max<int64_t>(1, (config.backend.fatal_flush_timeout.count() + 999) / 1000));
                quill::Backend::start<FrontendOptions>(options, signals);
            } else {
                quill::Backend::start(options);
            }
        }
        ctx.initialized = true;
        ctx.config = config;
        g_fatal_flush_timeout_ms.store(config.backend.fatal_flush_timeout.count());
        if (config.backend.crash_handler) std::set_terminate(&on_terminate);
        if (!config.backend.manual_backend) g_backend_mode.store(BackendMode::Thread);  // LogPoller sets Manual

        QUARK_TRY {
            for (const auto& [name, options] : config.loggers) {
                if (Logger* logger = Frontend::get_logger(name)) {
                    logger->set_log_level(options.level);
                    created_early.push_back(name);
                } else {
                    (void)create_logger_locked(ctx, name);  // throws on bad sink options
                }
            }
            if (config.backend.crash_handler && !config.backend.manual_backend) {
                Logger* quark = Frontend::get_logger(std::string{kQuarkLoggerName});
                if (quark == nullptr) quark = create_logger_locked(ctx, std::string{kQuarkLoggerName});
                Logger* crash = Frontend::create_or_get_logger(kCrashLoggerName, quark);
                const quill::LogLevel level = quark->get_log_level();
                crash->set_log_level(level == quill::LogLevel::None ? level : std::min(level, quill::LogLevel::Info));
            }
        } QUARK_CATCH_ALL() {
            // undo, so a corrected configuration can be retried
            g_backend_mode.store(BackendMode::None);
            if (!backend_taken && !config.backend.manual_backend) quill::Backend::stop();
            ctx.initialized = false;
            ctx.config = LogConfig{};
            QUARK_RETHROW;
        }
    }

    // Logged outside the lock, quark's logger may still have to be created.
    if (backend_taken) {
        QUARK_LOG_WARN("init_log(): the quill backend was already started elsewhere, [log] backend options ignored");
    }
    for (const auto& name : created_early) {
        QUARK_LOG_WARN("init_log(): logger {} was created before init_log(), only its level is applied", name);
    }
    QUARK_LOG_INFO("quark logging initialized: manual_backend={} loggers={}", config.backend.manual_backend,
                   config.loggers.size());
    return true;
}

bool init_log(const QuarkConfig& cfg, std::string_view section) {
    return init_log(LogConfig::load_from_config(cfg, section));
}

void shutdown_log() noexcept {
    auto& ctx = context();
    std::lock_guard lock{ctx.mtx};
    if (ctx.initialized && !ctx.manual_worker) {
        g_backend_mode.store(BackendMode::None);
        quill::Backend::stop();
    }
}

Logger* get_logger(std::string_view name) {
    const std::string key{name};
    if (Logger* logger = Frontend::get_logger(key)) {
        return logger;
    }
    auto& ctx = context();
    std::lock_guard lock{ctx.mtx};
    return create_logger_locked(ctx, key);
}

Logger* detail::quark_logger() {
    static Logger* logger = get_logger(kQuarkLoggerName);
    return logger;
}

// -----------------------------------------------------------------------------
// LogPoller
// -----------------------------------------------------------------------------
LogPoller::LogPoller() {
    auto& ctx = context();
    std::lock_guard lock{ctx.mtx};
    if (!ctx.manual_worker || ctx.poller_created) {
        QUARK_THROW(QuarkError{"LogPoller needs init_log() with manual_backend, and only one LogPoller may exist"});
    }
    // poll_once() requires a non-sleeping, non-yielding backend.
    quill::BackendOptions options;
    options.sleep_duration = std::chrono::nanoseconds{0};
    options.enable_yield_when_idle = false;
    set_common_backend_options(options);
    ctx.manual_worker->init(options);
    ctx.poller_created = true;
    g_poller_tid.store(current_tid());
    g_backend_mode.store(BackendMode::Manual);
}

LogPoller::~LogPoller() {
    g_backend_mode.store(BackendMode::None);
    context().manual_worker->shutdown();
}

void LogPoller::poll_once() {
    context().manual_worker->poll_one();
}

void LogPoller::poll_for(std::chrono::microseconds timeout) {
    context().manual_worker->poll(timeout);
}

// -----------------------------------------------------------------------------
// Fatal errors (QuarkError.h)
// -----------------------------------------------------------------------------
FatalHandler set_fatal_handler(FatalHandler handler) noexcept { return g_fatal_handler.exchange(handler); }

void fatal_error(const char* message, const char* file, int line) noexcept {
    if (g_in_fatal.exchange(true)) {
        std::abort();  // a fatal error while reporting one
    }
    if (message == nullptr) message = "(no message)";
    if (file != nullptr) {
        std::fprintf(stderr, "Quark fatal error: %s (%s:%d)\n", message, file, line);
    } else {
        std::fprintf(stderr, "Quark fatal error: %s\n", message);
    }
    std::fflush(stderr);
    if (FatalHandler handler = g_fatal_handler.load()) handler(message);
    flush_logs_on_fatal(message, file, line);
    std::abort();
}

QUARK_END_NAMESPACE
