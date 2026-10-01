#pragma once

#define QUILL_DISABLE_NON_PREFIXED_MACROS

#include "quark/Attributes.h"

#include <string>
#include <cstdint>

QUARK_BEGIN_NAMESPACE

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------
// quark's own log-level enum so users never need to name quill:: types.
enum class LogLevel : uint8_t {
    Debug,
    Info,
    Warning,
    Error,
};

// Callers set only what they care about and extensible for new options in the future.
// The two sink flags are independent: enable either or both.
struct LoggerConfig {
    // Defaults to Debug in Debug builds, Info in optimized (NDEBUG) builds.
    LogLevel level{LogLevel::Info};

    // --- Sink selection (independent; enable either or both) ---
    bool to_console{false};       // stdout, colorized — good for local/dev tails
    bool to_rotating_file{true};  // bounded on-disk logs — default for servers

    // --- RotatingFile options (ignored when to_rotating_file is false) ---
    // Base filename. Rotated files derive from this (e.g. quark.1.log, ...).
    std::string filename{"quark.log"};
    // Roll to a new file once the current one reaches this many bytes.
    std::size_t max_file_size_bytes{128 * 1024 * 1024};  // 128 MiB
    // Keep at most this many rotated files; older ones are removed. Bounds disk
    // usage for an infinite-loop process. 0 means "no count limit".
    std::uint32_t max_backup_files{10};
    // Also roll daily at this HH:MM (GmtTime). Empty disables time-based rotation.
    std::string daily_rotation_time{"00:00"};

    // --- Formatting ---
    // Quill pattern for the log line. Empty uses quark's default pattern.
    std::string pattern{};
};

QUARK_END_NAMESPACE

/**
// -----------------------------------------------------------------------------
// Logging Macros
// -----------------------------------------------------------------------------
// #define QUARK_LOG_DEBUG(fmt_str, ...)                                          \
//     do {                                                                       \
//         QUILL_LOG_DEBUG(::quark::quark_root_logger, fmt_str, ##__VA_ARGS__);   \
//     } while (0)

// #define QUARK_LOG_INFO(fmt_str, ...)                                           \
//     do {                                                                       \
//         QUILL_LOG_INFO(::quark::quark_root_logger, fmt_str, ##__VA_ARGS__);    \
//     } while (0)

// #define QUARK_LOG_WARN(fmt_str, ...)                                           \
//     do {                                                                       \
//         QUILL_LOG_WARNING(::quark::quark_root_logger, fmt_str, ##__VA_ARGS__); \
//     } while (0)

// #define QUARK_LOG_ERROR(fmt_str, ...)                                          \
//     do {                                                                       \
//         QUILL_LOG_ERROR(::quark::quark_root_logger, fmt_str, ##__VA_ARGS__);   \
//     } while (0)
*/
