#pragma once

// quark-internal logging macros. Not installed.
//
// Each source file may name its subsystem before including this header:
//   #define QUARK_LOG_COMPONENT "quark.config"
//   #include "detail/InternalLog.h"
//   QUARK_LOG_WARN("failed to parse {}: {}", path, reason);
//
// While logging runs, records go to quark's "quark" logger as `[quark.config] failed to parse ...`,
// filtered by its level. Before init_log() and after shutdown_log(), Warn and Error go to stderr.

#include "quark/QuarkError.h"
#include "quark/QuarkFormat.h"
#include "quark/QuarkLog.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#ifndef QUARK_LOG_COMPONENT
    #define QUARK_LOG_COMPONENT "quark"
#endif

QUARK_BEGIN_NAMESPACE

namespace detail {

// quark's own logger while logging runs, an invalid Logger otherwise. Lock-free.
[[nodiscard]] QUARK_API Logger quark_logger() noexcept;

QUARK_API void write_stderr(LogLevel level, const char* component, const char* file, uint32_t line,
                            std::string_view message) noexcept;

template <typename... Args>
QUARK_ATTRIBUTE_COLD QUARK_NOINLINE void log_to_stderr(LogLevel level, const char* component, const char* file,
                                                       uint32_t line, fmtlib::format_string<Args...> fmt,
                                                       Args&&... args) noexcept {
    QUARK_TRY {
        write_stderr(level, component, file, line, fmtlib::format(fmt, std::forward<Args>(args)...));
    }
    QUARK_CATCH_ALL() {
        write_stderr(level, component, file, line, "<quark: failed to format log message>");
    }
}

}  // namespace detail

QUARK_END_NAMESPACE

#if defined(QUARK_DISABLE_INTERNAL_LOG)
    #define QUARK_DETAIL_LOG(qlog_macro, level, fmt_str, ...) (void)0
#else
    #define QUARK_DETAIL_LOG(qlog_macro, level, fmt_str, ...)                                         \
        do {                                                                                          \
            if (const ::quark::Logger quark_internal_logger_ = ::quark::detail::quark_logger();        \
                quark_internal_logger_.valid()) {                                                     \
                qlog_macro(quark_internal_logger_, "[" QUARK_LOG_COMPONENT "] " fmt_str, ##__VA_ARGS__); \
            } else if constexpr (level >= ::quark::LogLevel::Warn) {                                  \
                ::quark::detail::log_to_stderr(level, QUARK_LOG_COMPONENT, __FILE__, __LINE__, fmt_str, \
                                               ##__VA_ARGS__);                                        \
            }                                                                                         \
        } while (0)
#endif

#define QUARK_LOG_DEBUG(fmt_str, ...) QUARK_DETAIL_LOG(QLOG_DEBUG, ::quark::LogLevel::Debug, fmt_str, ##__VA_ARGS__)
#define QUARK_LOG_INFO(fmt_str, ...)  QUARK_DETAIL_LOG(QLOG_INFO, ::quark::LogLevel::Info, fmt_str, ##__VA_ARGS__)
#define QUARK_LOG_WARN(fmt_str, ...)  QUARK_DETAIL_LOG(QLOG_WARN, ::quark::LogLevel::Warn, fmt_str, ##__VA_ARGS__)
#define QUARK_LOG_ERROR(fmt_str, ...) QUARK_DETAIL_LOG(QLOG_ERROR, ::quark::LogLevel::Error, fmt_str, ##__VA_ARGS__)
