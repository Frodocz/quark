#pragma once

#include "quark/Attributes.h"

#include <exception>
#include <string>

QUARK_BEGIN_NAMESPACE

QUARK_BEGIN_EXPORT

/**
 * Fatal errors. quark reports errors it can't return (constructors, configuration) by throwing
 * QuarkError; with QUARK_NO_EXCEPTIONS, and for uncaught exceptions once logging is initialized
 * (LogBackendOptions::crash_handler), they end in fatal_error() instead:
 *   1. the message goes to stderr,
 *   2. the handler set by set_fatal_handler() runs (alert, flatten positions...; keep it short),
 *   3. the message is logged as CRITICAL and the log queues are flushed, bounded by
 *      LogBackendOptions::fatal_flush_timeout so a stuck backend can't hang the process,
 *   4. std::abort().
 * Errors caused by external input while running (a malformed timestamp, a failed signature) have
 * non-throwing APIs (try_parse_timestamp, TimestampFormat::parse, Ed25519Signer::try_sign) and
 * never get here.
 */
using FatalHandler = void (*)(const char* message) noexcept;

/// Installs the handler run by fatal_error() before the logs are flushed; nullptr removes it.
/// Returns the previous handler.
QUARK_API FatalHandler set_fatal_handler(FatalHandler handler) noexcept;

/// Reports `message` as above and aborts. `file` may be null. Re-entering it (a fatal error while
/// reporting one) aborts at once.
[[noreturn]] QUARK_API void fatal_error(const char* message, const char* file = nullptr, int line = 0) noexcept;

/**
 * custom exception
 */
class QUARK_API QuarkError : public std::exception
{
public:
  explicit QuarkError(std::string s) : error_(static_cast<std::string&&>(s)) {}
  explicit QuarkError(const char* s) : error_(s) {}

  [[nodiscard]] const char* what() const noexcept override { return error_.data(); }

private:
    std::string error_;
};

QUARK_END_EXPORT

QUARK_END_NAMESPACE

#if defined(QUARK_NO_EXCEPTIONS)
    #define QUARK_REQUIRE(expression, error)                         \
        do {                                                         \
            if (!(expression)) [[unlikely]] {                        \
                ::quark::fatal_error((error), __FILE__, __LINE__);   \
            }                                                        \
        } while (0)

    // `ex` is a QuarkError expression: its message is reported, nothing is thrown.
    // A QUARK_CATCH block is dead code here and must not use its exception variable.
    #define QUARK_TRY if (true)
    #define QUARK_THROW(ex) ::quark::fatal_error((ex).what(), __FILE__, __LINE__)
    #define QUARK_CATCH(x) if (false)
    #define QUARK_CATCH_ALL() if (false)
    #define QUARK_RETHROW ::quark::fatal_error("rethrow in a build without exceptions", __FILE__, __LINE__)
#else
    #define QUARK_TRY try
    #define QUARK_THROW(ex) throw(ex)
    #define QUARK_CATCH(x) catch (x)
    #define QUARK_CATCH_ALL() catch (...)
    #define QUARK_RETHROW throw
#endif

// Checked in every build type, release included (unlike assert()): throws QuarkError, or is a
// fatal error with QUARK_NO_EXCEPTIONS.
#define QUARK_ASSERT(cond, ...)                                      \
    do {                                                             \
        if (!(cond)) {                                               \
            QUARK_THROW(quark::QuarkError{"Assert failed: " #cond}); \
        }                                                            \
    } while (0)
