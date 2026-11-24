# time checklist (RdtscClock / tscns / DateTime / LatencyRecorder / LatencyHistogram / RateLimiter / TimeScopedDedup)

Scope: `utils/RdtscClock.h`, `third_party/tscns`, `utils/DateTime.h` + `src/utils/DateTime.cpp`,
`LatencyRecorder.h`, `LatencyHistogram.h`, `RateLimiter.h`, `TimeScopedDedup.h`, `utils/System.h`
(the TSC/CPU-related parts).
Consumers: `RdtscClock::tscns()` stamps every MD message and order in both processes
(`WsSession` `recv_ns`, alphaframe `Engine.cpp`, tradeserver `TradeServer.cpp`, Binance `venue_ms()`);
`SlidingWindowLimiter` gates every order (`tradeserver/src/Router.cpp`); TimeScopedDedup dedups
across WsGroup sessions; DateTime `Date`/`TradingDay` in `alphaframe Strategy.h`.

## Checks
1. TSC assumptions: is invariant TSC checked (cpuid 0x80000007)? What happens on VMs (the targets are
   AWS EC2), under frequency scaling, or with TSC offsets between cores? How is it calibrated at
   start-up and how big is the error? `calibrate()` runs on the quill backend thread while the loop
   threads read with `rdns()`: are the parameters published so readers never see a torn or
   mismatched (base, multiplier) pair? Check tscns's sequence/ordering and what happens when calibrate is never called.
2. Per-call cost: `RdtscClock::instance()` is a function-local static, so every `tscns()` checks a
   guard (`__cxa_guard_acquire` fast path). Measure it, and consider constant initialisation or a
   thread-safe eager init. The first call blocks for ~20 ms to calibrate: make sure that happens
   before the hot loop.
3. Serialising the read: is `rdtsc` vs `rdtscp`/`lfence` chosen to fit the use (measuring latency
   vs taking a timestamp)? Can out-of-order execution make measurements too small?
4. Conversion: does the cycles→ns multiply-shift overflow after long uptimes (does it need a 128-bit
   intermediate or periodic rebasing)? Can a clock step back or an NTP step make durations negative,
   or wrap them as unsigned?
5. Clock choice: timeouts, heartbeats and rate limiting must use a monotonic clock; comparing with
   exchange timestamps needs wall time. `tscns()` follows the system clock, so it is **not
   monotonic** when the system clock steps. Check every use in RateLimiter, TimeScopedDedup,
   AliveMonitor, WsSession `max_recv_gap_ns`, TimerQueue and ExpBackoff.
6. DateTime: ms/us/ns unit mix-ups, negative epochs, leap years and month ends, how strict ISO8601
   parsing is and how it reports errors (`try_parse_timestamp` must not throw), format→parse round
   trips, non-thread-safe `localtime`/`gmtime`, zero allocation on parse paths, TradingDay rollover rules.
7. LatencyHistogram/Recorder: bucket edges and the overflow bucket, correctness when recording
   concurrently, whether recording itself adds overhead to the path being measured, off-by-one in percentiles.
8. RateLimiter/Dedup: overflow and clock steps in token refill or window sliding, burst limits,
   deterministic behaviour at window expiry and when capacity is reached (Dedup must not grow
   without bound during a burst). An exchange rate limit that is miscounted means rejected orders or a ban.

## Verification
- Unit tests with an injectable clock: steps backward, large jumps, values near overflow.
- Random differential tests of DateTime against a reference (Howard Hinnant's date algorithms or
  Python `datetime`), covering dates before 1970 and after 2038.
- TSan stress test with calibrate() on one thread and tscns() readers on others
  (`build_matrix.sh tsan`; with GCC, check whether a report is a fence false positive).
