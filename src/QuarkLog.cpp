#define QUARK_LOG_COMPONENT "quark.log"

#include "quark/QuarkLog.h"

#include "detail/InternalLog.h"
#include "quark/QuarkConfig.h"
#include "quark/QuarkError.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/sinks/ConsoleSink.h"
#include "quill/sinks/NullSink.h"
#include "quill/sinks/RotatingFileSink.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

#include <sys/syscall.h>
#include <unistd.h>

QUARK_BEGIN_NAMESPACE

namespace {

constexpr const char* kDefaultPattern{
    "%(time) [%(thread_id)] %(short_source_location:<28) %(log_level:<9) %(logger:<12) %(message)"};
constexpr const char* kDefaultTimestampPattern{"%Y-%m-%d %H:%M:%S.%Qus"};
constexpr const char* kConsoleSinkName{"quark_console"};
constexpr const char* kNullSinkName{"quark_null"};
constexpr const char* kBackendThreadName{"QuarkLogBackend"};

enum class State : uint8_t { Uninitialized, Running, Stopped };

struct LogContext {
    std::mutex mtx;
    std::atomic<State> state{State::Uninitialized};
    // Written by init_log() under mtx before state becomes Running (release), never modified
    // afterwards: readers that observe Running may use them without the lock.
    QueueType queue_type{QueueType::BoundedDropping};
    Logger root;
    std::map<std::string, Logger, std::less<>> loggers;
};

// Leaked on purpose so loggers stay usable from static destructors.
LogContext& context() {
    static auto* ctx = new LogContext;
    return *ctx;
}

constexpr quill::LogLevel to_quill_log_level(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug: return quill::LogLevel::Debug;
        case LogLevel::Info:  return quill::LogLevel::Info;
        case LogLevel::Warn:  return quill::LogLevel::Warning;
        case LogLevel::Error: return quill::LogLevel::Error;
        case LogLevel::Off:   return quill::LogLevel::None;
    }
    return quill::LogLevel::Info;
}

constexpr LogLevel from_quill_level(quill::LogLevel level) noexcept {
    if (level == quill::LogLevel::None) return LogLevel::Off;
    if (level <= quill::LogLevel::Debug) return LogLevel::Debug;
    if (level <= quill::LogLevel::Notice) return LogLevel::Info;
    if (level == quill::LogLevel::Warning) return LogLevel::Warn;
    return LogLevel::Error;
}

constexpr std::string_view to_string(QueueType type) noexcept {
    switch (type) {
        case QueueType::UnboundedBlocking: return "UnboundedBlocking";
        case QueueType::UnboundedDropping: return "UnboundedDropping";
        case QueueType::BoundedBlocking:   return "BoundedBlocking";
        case QueueType::BoundedDropping:   return "BoundedDropping";
    }
    return "Unknown";
}

std::vector<uint16_t> to_cpu_affinity(const std::vector<int64_t>& cpus) {
    std::vector<uint16_t> result;
    result.reserve(cpus.size());
    for (int64_t cpu : cpus) {
        if (cpu < 0 || cpu > std::numeric_limits<uint16_t>::max()) {
            QUARK_THROW(QuarkError{"LogOptions::cpu_affinity contains an invalid cpu id: " + std::to_string(cpu)});
        }
        result.push_back(static_cast<uint16_t>(cpu));
    }
    return result;
}

std::vector<std::shared_ptr<quill::Sink>> make_sinks(std::string_view name, const LogOptions& options) {
    std::vector<std::shared_ptr<quill::Sink>> sinks;

    if (options.to_console) {
        sinks.push_back(quill::Frontend::create_or_get_sink<quill::ConsoleSink>(kConsoleSinkName));
    }

    if (options.to_rotating_file) {
        quill::RotatingFileSinkConfig cfg;
        cfg.set_open_mode('a');
        cfg.set_timezone(quill::Timezone::GmtTime);
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
        const std::string filename =
            options.filename.empty() ? "logs/" + std::string{name} + ".log" : options.filename;
        sinks.push_back(quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(filename, cfg));
    }

    // Both sinks disabled: keep a valid logger that discards everything.
    if (sinks.empty()) {
        sinks.push_back(quill::Frontend::create_or_get_sink<quill::NullSink>(kNullSinkName));
    }
    return sinks;
}

template <typename T>
void assign(const QuarkConfig& cfg, const std::string& path, T& out) {
    if (auto value = cfg.get<T>(path)) {
        out = std::move(*value);
    }
}

}  // namespace

// -----------------------------------------------------------------------------
// LogOptions / LogConfig
// -----------------------------------------------------------------------------
namespace {

std::string join_path(std::string_view section, std::string_view field) {
    std::string path{section};
    path += '.';
    path += field;
    return path;
}

// Keys of a [log.<name>] table, and of the [log] table.
constexpr std::string_view kLoggerKeys[] = {"to_console", "to_rotating_file", "level", "pattern", "filename",
                                            "max_file_size_bytes", "max_backup_files", "daily_rotation_time"};
constexpr std::string_view kGlobalKeys[] = {"queue_type", "cpu_affinity"};

template <std::size_t N>
std::string_view find_present_key(const QuarkConfig& cfg, std::string_view section,
                                  const std::string_view (&keys)[N]) {
    for (std::string_view key : keys) {
        if (cfg[join_path(section, key)].has_value()) {
            return key;
        }
    }
    return {};
}

}  // namespace

LogOptions LogOptions::load_from_config(const QuarkConfig& cfg, std::string_view section) {
    const auto key = [section](std::string_view field) { return join_path(section, field); };

    LogOptions options;
    assign(cfg, key("to_console"), options.to_console);
    assign(cfg, key("to_rotating_file"), options.to_rotating_file);
    if (auto level = cfg.get<std::string>(key("level"))) {
        options.level = parse_log_level(*level);
    }
    assign(cfg, key("pattern"), options.pattern);
    assign(cfg, key("filename"), options.filename);
    assign(cfg, key("max_file_size_bytes"), options.max_file_size_bytes);
    assign(cfg, key("max_backup_files"), options.max_backup_files);
    assign(cfg, key("daily_rotation_time"), options.daily_rotation_time);
    return options;
}

LogConfig LogConfig::load_from_config(const QuarkConfig& cfg, std::string_view section) {
    LogConfig config;
    if (auto queue_type = cfg.get<std::string>(join_path(section, "queue_type"))) {
        config.global.queue_type = parse_queue_type(*queue_type);
    }
    if (auto cpus = cfg.get<QuarkConfigArray>(join_path(section, "cpu_affinity"))) {
        for (const auto& cpu : *cpus) {
            if (auto id = cpu.get<int64_t>()) {
                config.global.cpu_affinity.push_back(*id);
            }
        }
    }
    if (auto key = find_present_key(cfg, section, kLoggerKeys); !key.empty()) {
        QUARK_LOG_WARN("[{}] {} ignored: per-logger options belong in a [{}.<logger name>] table", section, key,
                       section);
    }

    for (std::string& name : cfg.table_names(section)) {
        const std::string logger_section = join_path(section, name);
        if (auto key = find_present_key(cfg, logger_section, kGlobalKeys); !key.empty()) {
            QUARK_LOG_WARN("[{}] {} ignored: it is a global option, set it in [{}]", logger_section, key, section);
        }
        config.loggers.emplace(std::move(name), LogOptions::load_from_config(cfg, logger_section));
    }
    return config;
}

// -----------------------------------------------------------------------------
// Logger
// -----------------------------------------------------------------------------
namespace detail {

struct LoggerFactory {
    // Caller holds ctx.mtx.
    static Logger create_locked(LogContext& ctx, const std::string& name, const LogOptions& options) {
        if (auto it = ctx.loggers.find(name); it != ctx.loggers.end()) {
            return it->second;
        }

        auto sinks = make_sinks(name, options);
        const quill::PatternFormatterOptions pattern{
            options.pattern.empty() ? std::string{kDefaultPattern} : options.pattern, kDefaultTimestampPattern};

        quill::detail::LoggerBase* base = visit_queue_type(ctx.queue_type, [&](auto type) -> quill::detail::LoggerBase* {
            return quill::FrontendImpl<FrontendOptions<decltype(type)::value>>::create_or_get_logger(
                name, std::move(sinks), pattern);
        });
        base->set_log_level(to_quill_log_level(options.level));

        Logger logger{base, ctx.queue_type};
        ctx.loggers.emplace(name, logger);
        return logger;
    }
};

}  // namespace detail

std::string_view Logger::name() const noexcept {
    return logger_ ? std::string_view{logger_->get_logger_name()} : std::string_view{};
}

LogLevel Logger::level() const noexcept {
    return logger_ ? from_quill_level(logger_->get_log_level()) : LogLevel::Info;
}

void Logger::set_level(LogLevel level) noexcept {
    if (!logger_) {
        return;
    }
    logger_->set_log_level(to_quill_log_level(level));
}

void Logger::flush() const {
    if (!logger_ || context().state.load(std::memory_order_acquire) != State::Running) {
        return;
    }
    detail::visit_queue_type(queue_type_, [this](auto type) {
        static_cast<detail::LoggerImpl<decltype(type)::value>*>(logger_)->flush_log();
    });
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------
bool init_log(const LogConfig& config) {
    auto& ctx = context();
    std::lock_guard lock{ctx.mtx};
    if (ctx.state.load(std::memory_order_relaxed) != State::Uninitialized) {
        QUARK_LOG_WARN("init_log() ignored: logging was already initialized");
        return false;
    }

    quill::BackendOptions backend_options;
    backend_options.thread_name = kBackendThreadName;
    backend_options.cpu_affinity = to_cpu_affinity(config.global.cpu_affinity);
    quill::Backend::start(backend_options);
    ctx.queue_type = config.global.queue_type;

    // quark's own logger first, with defaults unless [log.quark] is configured.
    const std::string quark_name{kQuarkLoggerName};
    const auto quark_it = config.loggers.find(quark_name);
    const LogOptions quark_options = quark_it != config.loggers.end() ? quark_it->second : LogOptions{};
    ctx.root = detail::LoggerFactory::create_locked(ctx, quark_name, quark_options);

    std::string names{quark_name};
    for (const auto& [name, options] : config.loggers) {
        if (name != quark_name) {
            (void)detail::LoggerFactory::create_locked(ctx, name, options);
            names += ", " + name;
        }
    }
    // Publishes queue_type, root and loggers to lock-free readers.
    ctx.state.store(State::Running, std::memory_order_release);

    QUARK_LOG_INFO("quark logging initialized: queue_type={} initial_queue_capacity={} loggers=[{}]",
                   to_string(ctx.queue_type), kLogInitialQueueCapacity, names);
    return true;
}

bool init_log(const QuarkConfig& cfg, std::string_view section) {
    return init_log(LogConfig::load_from_config(cfg, section));
}

void shutdown_log() noexcept {
    auto& ctx = context();
    std::lock_guard lock{ctx.mtx};
    if (ctx.state.exchange(State::Stopped, std::memory_order_acq_rel) == State::Running) {
        // quark's own records go to stderr from now on.
        // Drains every frontend queue before the backend thread exits.
        quill::Backend::stop();
    }
}

Logger get_logger(std::string_view name) noexcept {
    auto& ctx = context();
    if (ctx.state.load(std::memory_order_acquire) != State::Running) {
        return {};
    }
    if (auto it = ctx.loggers.find(name); it != ctx.loggers.end()) {
        return it->second;
    }
    QUARK_LOG_WARN("get_logger({}): no such logger, declare it in a [log.{}] table", name, name);
    return {};
}

// -----------------------------------------------------------------------------
// quark-internal logging, see detail/InternalLog.h
// -----------------------------------------------------------------------------
Logger detail::quark_logger() noexcept {
    auto& ctx = context();
    return ctx.state.load(std::memory_order_acquire) == State::Running ? ctx.root : Logger{};
}

void detail::write_stderr(LogLevel level, const char* component, const char* file, uint32_t line,
                          std::string_view message) noexcept {
    static constexpr char kLevelChar[] = {'D', 'I', 'W', 'E', 'O'};
    thread_local const auto tid = static_cast<unsigned long long>(::syscall(SYS_gettid));

    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    const std::time_t secs = std::chrono::duration_cast<std::chrono::seconds>(since_epoch).count();
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(since_epoch).count() % 1'000'000;
    std::tm tm{};
    ::localtime_r(&secs, &tm);

    std::string_view filename{file};
    if (const auto pos = filename.find_last_of('/'); pos != std::string_view::npos) {
        filename.remove_prefix(pos + 1);
    }

    // `W20261008 12:34:56.789012 4242 QuarkConfig.cpp:42] [quark.config] message`. One fprintf per
    // record: stderr is unbuffered, so lines from different threads don't interleave.
    std::fprintf(stderr, "%c%04d%02d%02d %02d:%02d:%02d.%06lld %llu %.*s:%u] [%s] %.*s\n",
                 kLevelChar[static_cast<uint8_t>(level) % sizeof(kLevelChar)], tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long long>(micros), tid,
                 static_cast<int>(filename.size()), filename.data(), line, component,
                 static_cast<int>(message.size()), message.data());
}

QUARK_END_NAMESPACE
