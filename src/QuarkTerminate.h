#pragma once

#include "quark/Attributes.h"

QUARK_BEGIN_NAMESPACE

namespace detail {

/// std::terminate handler installed by init_log() (LogBackendOptions::crash_handler): reports an
/// uncaught exception's message, or that terminate was called, through fatal_error().
[[noreturn]] void on_terminate() noexcept;

}  // namespace detail

QUARK_END_NAMESPACE
