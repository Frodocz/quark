# lessons-learned (defect patterns actually found)

Read this before every review and check these patterns first. Each entry must come from a real
defect, not a guess. Entries are added only after the user confirms them. At the end of a review
the reviewer may propose candidates but must not edit this file.

## Entry format

### LL-NNN One-line title
- Module / file:
- Symptom and trigger:
- Root cause:
- How to check (command, grep pattern, test method):
- Fix commit / date:

## Entries

### LL-001 Internal headers must open namespaces with QUARK_BEGIN_NAMESPACE
- Module / file: `src/net/TlsInternal.h`, `src/crypto/OpenSslError.h`
- Symptom and trigger: clang 14 builds failed to link the TLS code (undefined
  `quark::net::detail::openssl_error_string()` / `on_new_tls_session()`); GCC builds linked fine.
- Root cause: `namespace quark::net::detail {` written by hand instead of `QUARK_BEGIN_NAMESPACE` +
  `namespace net::detail`. GCC treats it as reopening the inline `quark::v1::net::detail`; clang 14 opens
  a separate unversioned `quark::net::detail`, so declarations and definitions end up in different namespaces.
  Even when it links (inline functions), the symbol escapes the `v1` version guard.
- How to check: `grep -rn "^namespace quark" include src` (only `quark::fmtlib` in QuarkFormat.h is
  intentional); ctest `quark_test_symbols_versioned` fails on any `quark::` symbol outside `quark::v1`;
  build once with clang.
- Fix commit / date: 4371aba (2026-11-25)

### LL-002 "Success" returned after a callback already reported the failure
- Module / file: `net/HttpClient.h` (`request()`), and `HttpPool` through it
- Symptom and trigger: the peer reset an idle keep-alive connection; the next `request()` failed while
  sending, called `on_error` from inside, then returned `Submit::Ok`. Trading's callers set
  "busy" on `Ok` after their `on_error` had already cleared it: REST snapshots (alphaframe) and REST
  queries (tradeserver) stopped for the life of the process.
- Root cause: a synchronous failure path (`fail()` → `on_error_()`) reachable from inside an API call
  whose return value the caller trusts. The two channels disagreed about what happened.
- How to check: for every call that can tear down (`fail()`, `teardown()`, write/flush errors), see
  whether it is reachable from a public call that then returns success. An accepted request must get
  exactly one later callback; failures inside the call are deferred (a zero-delay timer) or returned.
  Test: make the failure synchronous (RST before the call, without stepping the loop) and check that
  no callback runs inside the call.
- Fix commit / date: NET-01 (2026-11-25)

### LL-003 Recursion on network input
- Module / file: `net/HttpCodec.h` (`parse_http_response`)
- Symptom and trigger: one stack frame per 1xx interim response; ~58k "100 Continue" responses in a
  1 MiB receive buffer (the tradeserver's size) overflowed the stack: SIGSEGV.
- Root cause: a parser that calls itself once per repeated element of untrusted input, with work after
  the call (no tail call), so depth grows with the input.
- How to check: `grep` parsers for self-calls; replace with a loop and a cap. Fuzz with inputs as large
  as the production buffers (`-max_len` of 4 KiB missed this one); add a test with a buffer-filling
  flood of the repeated element.
- Fix commit / date: NET-02 (2026-11-25)

### LL-004 Typed lookups that return the default on a type mismatch
- Module / file: `QuarkConfig.h` (`get_or`, `get`, `QuarkConfigValue::get`)
- Symptom and trigger: `max_position = 3` read with `get_or<double>(..., 0.005)` gave 0.005. TOML writes
  `3` as an integer and `3.0` as a float; trading's `conf/backtest_example.toml` had `queue_power = 2`
  read as a double, right only because the default was 2.0. `cpu = "3"` read as an integer gave the
  default (unpinned) just as silently. The tradeserver had worked around it for its risk limits.
- Root cause: an accessor that treats "present but another type" like "missing" and returns the
  caller's default; with no error, a wrong parameter looks like a deliberate one.
- How to check: for every `get_or`-style accessor (config, JSON), test a present key of the wrong type
  and an integer read as a float; grep consumers for `get_or<double>` and for keys with integer values
  in their config files.
- Fix commit / date: RUN-01 (2026-11-26)

### LL-005 Constructors that preallocate, called per request
- Module / file: `utils/JsonParser.h` (constructor + `warm_up()`); consumer `tradeserver/src/BinanceUsdm.cpp:81`,
  `BitgetUta.cpp:201,223`
- Symptom and trigger: the tradeserver built a `JsonParser` per REST response, on the loop thread that also
  sends orders. Construction preallocates and pre-faults buffers for its capacity: 1.5 us at 1 KB, 95 us
  at 64 KB, 1.7 ms at 1 MB, before any parsing. Per response, a new `JsonParser(1024)` cost ~1.8 us where
  a reused one parses the same 500-byte response in ~150 ns (14x).
- Root cause: a type whose constructor is meant to be paid once (preallocation, pre-faulting) looks like
  any cheap local at the call site; nothing stops it being built per message.
- How to check: grep consumers for preallocating types (JsonParser, pools, arenas, histograms, BufferPool)
  constructed inside per-message or per-response functions; benchmark construction vs reuse
  (`BM_JsonParserConstruct`, `BM_JsonParsePerResponse*`); document "construct once" on such types.
- Fix commit / date: PARSE-01 (2026-11-26; quark documents it, the trading change is pending)

### LL-006 An outlier filter that can lock in a stale value
- Module / file: `utils/RdtscClock.h` (`detail::SteadyTscClock`, the rate behind `steady_ns()`)
- Symptom and trigger: calibration rejected every rate sample more than 1% away from the current rate as
  an outlier. After a lasting TSC rate change above 1% (or a bad first rate), every later sample was
  "too far", so the rate froze and `steady_ns()` drifted from wall time without bound (a 2% change:
  -11.8 s after 10 min). Found in review of a fix made earlier the same day.
- Root cause: a filter that compares each sample only with the value it protects, and never lets
  samples change that value: a genuine, lasting change looks like an endless stream of outliers.
- How to check: for every "ignore samples too far from the current estimate" rule, test a lasting
  step beyond the threshold and check the estimate follows. Accept a change confirmed by consecutive
  agreeing samples (here 3 within 0.1%), and still test that alternating outliers are not adopted.
  Tests: `TestSteadyTscClock.LastingRateChangeIsAdopted`, `AlternatingOutliersAreNotAdopted`.
- Fix commit / date: TIME-04 (2026-11-26, not yet committed)
