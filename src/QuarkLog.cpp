#include "quark/QuarkLog.h"

#include "quark/QuarkConfig.h"
#include "quark/QuarkError.h"

#include "quill/Backend.h"
#include "quill/sinks/ConsoleSink.h"
#include "quill/sinks/NullSink.h"
#include "quill/sinks/RotatingFileSink.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

static_assert(quill::VersionMajor == 13 && quill::VersionMinor == 0 && quill::VersionPatch == 0,
              "quark and every module of the process must use the same quill version, see QUARK_QUILL_VERSION");

QUARK_BEGIN_NAMESPACE

namespace {

constexpr const char* kDefaultPattern{
    "%(time) [%(thread_id)] %(short_source_location:<20) %(log_level:<8) %(logger:<6) %(message)"};
constexpr const char* kDefaultTimestampPattern{"%Y-%m-%d %H:%M:%S.%Qns"};
constexpr const char* kConsoleSinkName{"quark_console"};
constexpr const char* kNullSinkName{"quark_null"};
constexpr const char* kBackendThreadName{"QuarkLogBackend"};

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

std::vector<std::shared_ptr<quill::Sink>> make_sinks(const std::string& name, const LogOptions& options) {
    std::vector<std::shared_ptr<quill::Sink>> sinks;

    if (options.to_console) {
        sinks.push_back(Frontend::create_or_get_sink<quill::ConsoleSink>(kConsoleSinkName));
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
        options.level = quill::loglevel_from_string(*level);
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
            quill::Backend::start(options);
        }
        ctx.initialized = true;
        ctx.config = config;

        for (const auto& [name, options] : config.loggers) {
            if (Logger* logger = Frontend::get_logger(name)) {
                logger->set_log_level(options.level);
                created_early.push_back(name);
            } else {
                (void)create_logger_locked(ctx, name);
            }
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
    ctx.manual_worker->init(options);
    ctx.poller_created = true;
}

LogPoller::~LogPoller() {
    context().manual_worker->shutdown();
}

void LogPoller::poll_once() {
    context().manual_worker->poll_one();
}

void LogPoller::poll_for(std::chrono::microseconds timeout) {
    context().manual_worker->poll(timeout);
}

QUARK_END_NAMESPACE
