#pragma once

#include "quark/Attributes.h"

QUARK_BEGIN_NAMESPACE

namespace net {

#if defined(QUARK_USE_FSTACK)

/// Initializes F-Stack (DPDK EAL, the DPDK port and the FreeBSD TCP/IP stack) once per process,
/// before any quark::net socket is created. `argv` must contain "--conf <f-stack.conf>": there is
/// no default configuration. Must run as root (DPDK owns the NIC). Returns false on failure.
QUARK_API bool init_fstack(int argc, char** argv);

/// True between a successful init_fstack() and the end of EventLoop::run(): F-Stack tears DPDK
/// down when its loop returns, after which sockets are closed without calling into it.
[[nodiscard]] QUARK_API bool fstack_active() noexcept;

namespace detail {
QUARK_API void set_fstack_active(bool active) noexcept;
}  // namespace detail

#endif

} // namespace net

QUARK_END_NAMESPACE
