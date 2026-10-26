#pragma once

#include "quark/Attributes.h"

#include <exception>
#include <string>

#if defined(QUARK_NO_EXCEPTIONS)
    #include <cstdio>
    #include <cstdlib>

    #define QUARK_REQUIRE(expression, error)                             \
        do                                                               \
        {                                                                \
            if (!(expression)) [[unlikely]]                              \
            {                                                            \
                std::fprintf(stderr, "Quark fatal error: %s (%s:%d)\n", \
                    error, __FILE__, __LINE__);                          \
                std::abort();                                            \
            }                                                            \
        } while (0)

    #define QUARK_TRY if (true)
    #define QUARK_THROW(ex) QUARK_REQUIRE(false, ex.what())
    #define QUARK_CATCH(x) if (false)
    #define QUARK_CATCH_ALL() if (false)
    #define QUARK_RETHROW std::abort()
#else
    #define QUARK_TRY try
    #define QUARK_THROW(ex) throw(ex)
    #define QUARK_CATCH(x) catch (x)
    #define QUARK_CATCH_ALL() catch (...)
    #define QUARK_RETHROW throw
#endif

QUARK_BEGIN_NAMESPACE

QUARK_BEGIN_EXPORT

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

#define QUARK_ASSERT(cond, ...)                                      \
    do {                                                             \
        if (!(cond)) {                                               \
            QUARK_THROW(quark::QuarkError{"Assert failed: " #cond}); \
        }                                                            \
    } while (0)

QUARK_END_NAMESPACE
