# Tuning a host for quark

quark's hot paths avoid allocation, locks and system calls where they can; what's left is the
machine. This guide lists the settings that matter for a busy-polling quark process on Linux
(EC2 with ENA in the examples), what each one costs, and how to check it. Apply them on the hosts
that run the latency-critical loops; measure before and after (`LatencyHistogram`, the
benchmarks under `benchmarks/`).

Throughout, the example reserves cores **2-7** for event loops and leaves **0-1** (and the rest)
for the kernel, IRQs and everything else. Adapt the ranges to your instance (`lscpu -e`).

## 1. Dedicate cores to the event loops

A busy-polling loop should own its core: no other task, no timer ticks, no RCU callbacks, no IRQs.

Kernel command line (`/etc/default/grub`, `GRUB_CMDLINE_LINUX`, then `update-grub` and reboot):

```
isolcpus=managed_irq,domain,2-7 nohz_full=2-7 rcu_nocbs=2-7 irqaffinity=0-1 skew_tick=1
```

- `isolcpus` keeps the scheduler from placing other tasks there; `nohz_full` stops the periodic
  tick while one task runs; `rcu_nocbs` moves RCU callbacks off; `irqaffinity` sets the default
  IRQ cores.
- `vm.stat_interval = 10` (sysctl) reduces the per-core vmstat work that still interrupts
  `nohz_full` cores.

In the application:

```cpp
quark::pin_thread_to_cpu(2);          // warns if cpu 2 isn't isolated
quark::set_thread_name("io-loop-0");
```

Check: `cat /sys/devices/system/cpu/isolated /sys/devices/system/cpu/nohz_full`, and that only
your thread runs there: `ps -eLo psr,comm | awk '$1==2'`.

## 2. Real-time priority (optional) and RT throttling

`set_thread_realtime(priority)` (SCHED_FIFO) keeps kernel threads that still land on the core
from preempting the loop. On an isolated core it is rarely needed; if you use it:

- Linux throttles real-time tasks by default (`kernel.sched_rt_runtime_us = 950000`): a
  busy-polling FIFO thread is **paused for ~50 ms every second**. Disable throttling with
  `sysctl kernel.sched_rt_runtime_us=-1` (quark logs a warning when it is on).
- It needs `CAP_SYS_NICE` (systemd: `LimitRTPRIO=99`, or `AmbientCapabilities=CAP_SYS_NICE`).

## 3. Interrupts

NIC interrupts on a loop core stall it; interrupt moderation delays packets.

- Stop `irqbalance` (it moves IRQs back): `systemctl disable --now irqbalance`.
- Pin the NIC's IRQs to housekeeping cores:
  `for irq in $(grep ens5 /proc/interrupts | cut -d: -f1); do echo 0-1 > /proc/irq/$irq/smp_affinity_list; done`
- ENA interrupt moderation: `ethtool -C ens5 adaptive-rx off rx-usecs 0` (adaptive RX is on by
  default and adds delay under load).
- Fewer queues means fewer IRQs to place: `ethtool -L ens5 combined 2` (size to your traffic).

Check: `ethtool -c ens5`, `grep ens5 /proc/interrupts` (counts should only grow on cores 0-1).

## 4. Kernel busy polling (kernel sockets)

In busy-poll mode quark calls `recv()` on every connection each step: an idle connection costs
one syscall per step (~630 ns measured on a Xeon 8259CL VM with the default mitigations), while
epoll mode pays one `epoll_wait()` per step (~550 ns) however many connections are idle. So
busy-poll the few latency-critical connections (one loop each) and put the many mostly idle
ones (e.g. market data fan-in) on an epoll-mode loop. With
`sysctl net.core.busy_read=50` (microseconds) the kernel also polls the NIC queue inside that
`recv()` instead of waiting for the interrupt and softirq, which can cut receive latency; the
benefit depends on the driver and traffic, so measure it. `net.core.busy_poll` does the same for
`poll()`/`epoll_wait()` (epoll mode).

## 5. Memory

- `quark::lock_all_memory()` (mlockall) keeps every page resident; give the process the limit:
  systemd `LimitMEMLOCK=infinity` (shell: `ulimit -l unlimited`).
- Huge pages for `MemoryArena`, `BufferPool`, the logger queues and F-Stack:
  `sysctl vm.nr_hugepages=1024` (2 MiB each; persist in `/etc/sysctl.d/`).
  `MemoryArena` uses them for arenas of 2 MiB and more, and falls back to transparent huge pages
  (`/sys/kernel/mm/transparent_hugepage/enabled` should be `madvise`) or normal pages.
- Create pools, sessions and arenas at startup: steady state then never allocates or faults.

Check: `grep -E "HugePages_(Total|Free)" /proc/meminfo`, `ulimit -l`.

## 6. Clock

`RdtscClock` and every quark timestamp read the TSC. The kernel must trust it too:
`/sys/devices/system/clocksource/clocksource0/current_clocksource` should be `tsc`, and the CPU
must report `constant_tsc nonstop_tsc` (`grep -o 'constant_tsc\|nonstop_tsc' /proc/cpuinfo | sort -u`).

## 7. CPU power states

Deep C-states add microseconds of wake-up latency; frequency scaling adds jitter. Busy polling
keeps the loop cores out of idle anyway. Where the guest controls them (bare-metal and some large
EC2 instances), `intel_idle.max_cstate=1 processor.max_cstate=1` on the kernel command line keeps
the other cores shallow too; on most virtualized instances the hypervisor decides (no
`/sys/devices/system/cpu/cpu0/cpufreq`).

## 8. Network path

- TLS: quark prefers AES-128-GCM for TLS 1.3 and resumes sessions on reconnect; keep the
  `TlsContext` per loop.
- Pick the instance's availability zone by measured round-trip time to the venue, and check
  `ethtool -S ens5 | grep allowance_exceeded` (EC2 per-ENI packet and connection limits).
- For the lowest latency, run the hot connections on F-Stack (`QUARK_USE_FSTACK=ON`, see the
  README) on a dedicated ENI; the instance keeps its primary ENI for SSH, DNS and the kernel.

## 9. Process setup checklist

```cpp
int main(int argc, char** argv) {
    quark::install_crash_handler();     // stack trace on SIGSEGV & co., core dump kept
    quark::install_shutdown_handler();  // first SIGINT/SIGTERM: stop gracefully
    quark::lock_all_memory();
    // ... create pools, arenas, TLS contexts, sessions ...
    quark::pin_thread_to_cpu(2);
    quark::net::EventLoop loop;
    loop.set_step_hook([&] {
        if (quark::shutdown_requested()) loop.stop();
    });
    loop.run();
}
```
