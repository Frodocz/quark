#pragma once

#include "quark/Attributes.h"

#include <pthread.h>

#include <string_view>

QUARK_BEGIN_NAMESPACE

// ---------------------------------------------------------------- threads and CPUs

/// Pins `thread` to `cpu`. Logs a warning when the CPU isn't isolated from the scheduler
/// (isolcpus): other tasks can still preempt the thread there. Returns false on failure.
QUARK_API bool pin_thread_to_cpu(int cpu, pthread_t thread = pthread_self());

/// Real-time FIFO scheduling at `priority` (1-99). Needs CAP_SYS_NICE (or root); returns false
/// otherwise. A busy-polling FIFO thread starves everything else on its CPU: pin it to an
/// isolated core. Warns when real-time throttling (kernel.sched_rt_runtime_us != -1) would pause
/// it periodically.
QUARK_API bool set_thread_realtime(int priority, pthread_t thread = pthread_self());

/// Thread name shown by top/ps/gdb (at most 15 characters, longer names are truncated).
QUARK_API bool set_thread_name(std::string_view name, pthread_t thread = pthread_self());

/// The CPU the calling thread runs on right now.
[[nodiscard]] QUARK_API int current_cpu() noexcept;

/// Whether `cpu` is in the kernel's isolated set (/sys/devices/system/cpu/isolated).
[[nodiscard]] QUARK_API bool cpu_isolated(int cpu);

/// Whether `cpu` is listed in a kernel CPU list such as "2-5,8" (as in /sys/devices/system/cpu).
[[nodiscard]] QUARK_API bool cpu_list_contains(std::string_view list, int cpu) noexcept;

// ---------------------------------------------------------------- memory

/// Locks all current and future pages of the process in RAM (mlockall): nothing is ever paged out.
/// Needs RLIMIT_MEMLOCK headroom (or CAP_IPC_LOCK). Returns false on failure.
QUARK_API bool lock_all_memory();

// ---------------------------------------------------------------- signals

/// On SIGSEGV, SIGBUS, SIGFPE, SIGILL and SIGABRT, writes the signal and a stack trace to stderr
/// using async-signal-safe calls only, then re-raises the signal with its default action (so a
/// core dump is still produced). Records still queued in the async logger may be lost.
QUARK_API void install_crash_handler();

/// The first SIGINT or SIGTERM sets shutdown_requested() (check it e.g. in an event loop's step
/// hook and stop gracefully); a second one terminates the process right away.
QUARK_API void install_shutdown_handler();

[[nodiscard]] QUARK_API bool shutdown_requested() noexcept;

QUARK_END_NAMESPACE
