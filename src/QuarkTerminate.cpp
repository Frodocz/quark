// Compiled with exceptions even with QUARK_NO_EXCEPTIONS (CMakeLists.txt): quark itself doesn't
// throw then, but the application's own code may (trading's does), and the reason of an exception
// that escapes it belongs in the log. This file only inspects an exception that already exists;
// nothing here throws a new one.
#include "QuarkTerminate.h"

#include "quark/QuarkError.h"

#include <cstdio>
#include <exception>

QUARK_BEGIN_NAMESPACE

void detail::on_terminate() noexcept {
#if defined(__cpp_exceptions)
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

QUARK_END_NAMESPACE
