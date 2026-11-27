# Changelog

All notable changes to quark are listed here. quark follows [semantic versioning](https://semver.org):
the major version changes with incompatible API or ABI changes, so `find_package(quark 1.0)` accepts
any 1.x and `libquark.so.1` stays binary compatible across 1.x releases.

## [Unreleased]

### Changed

- `TradingDay::init()` and `TradingDay(int64_t)` accept instants before 1970 (floor, like `get_date()`); they
  used to refuse them, and the constructor then silently left date 0. `init()` now returns false only within
  a day of the int64 limits, where the previous/next day's start would overflow (it did, for INT64_MAX).
- `SlidingWindowLimiter` refuses a window <= 0 and a limit of 0 or UINT32_MAX at construction (QuarkError,
  fatal with `QUARK_NO_EXCEPTIONS`; they were only `assert()`ed). `try_acquire(now, 0)` succeeds and takes nothing.
- WsGroup's dedup window and delay measurement run on the loop's clock (`EventLoop::now_ns()`), so a
  system clock step can't expire the window (letting late copies through) or stretch it.
- `init_log()` warns when the CPU reports no invariant TSC or the kernel clocksource isn't `tsc`.
- `HmacSha256::sign()` / `sign_hex()` / `sign_base64()` on a moved-from object throw QuarkError (fatal with
  `QUARK_NO_EXCEPTIONS`) instead of dereferencing null in Release builds (it was only `assert()`ed).
- `JsonParser` documents that construction is a start-up cost (about 1.5 us at 1 KB, 95 us at 64 KB, 1.7 ms
  at 1 MB): keep one parser and reuse it, don't construct one per message or REST response.
- `EventLoop::now_ns()` (and so every TimerQueue deadline, HttpClient timeout and WsSession liveness check)
  is `RdtscClock::steady_ns()` instead of `tscns()`: still nanoseconds since the epoch, but a step of the
  system clock no longer jumps it (with `tscns()`, a 30 s forward step made every deadline under 30 s fire
  at once, and a backward step blinded the liveness checks). WsSession measures receive gaps and ping RTT
  on the loop's clock; `WsMessage::recv_ns` stays `tscns()` (wall time).
- `parse_http_response()` (HttpClient's parser) is strict about framing (RFC 9112 6.3): conflicting
  Content-Lengths, Content-Length together with Transfer-Encoding, a Transfer-Encoding whose last coding
  isn't `chunked`, a status below 100, whitespace before a header's colon and obsolete line folding are
  malformed. 204 and 304 responses never have a body. `Connection` is read as a token list (`close` wins).
- HttpClient: a keep-alive connection the server closes while idle, after it served a response, is
  reopened right away with the backoff reset (it grew with every such close: up to 30 s of NotReady).
- HttpClient: without `auto_reconnect`, a failed connect or a connect timeout reports `on_error`.
- WsSession and HttpClient refuse `reconnect_initial_ns <= 0` and `reconnect_max_ns < reconnect_initial_ns`
  at construction (QuarkError, fatal with `QUARK_NO_EXCEPTIONS`).
- **(breaking)** `ShmDirectory::alive()` returns `std::optional<std::vector<std::string>>`: `nullopt` when the
  scan fails (no fd left, a permission error), instead of an empty list that callers took for "every client
  exited". A missing directory is still an empty list. `is_alive()` is replaced by `liveness()`, which returns
  `Liveness::{Dead, Alive, Unknown}` (`Unknown`: the probe failed).
- **(breaking)** `FullDuplexShm` accepts only files on tmpfs or hugetlbfs (e.g. under
  `FullDuplexShm::kDefaultDir`, `/dev/shm`): on a disk filesystem every queue page page-faulted on its next
  write after each writeback (~every 30 s). Other locations are refused.
- **(breaking)** `MMapFile` with `create == false` no longer resizes an existing file whose size differs from
  `Options::size`: it fails. `FullDuplexShm` therefore refuses a peer's files of another queue size instead of
  shrinking them under the peer, which then died of SIGBUS.

### Added

- `RdtscClock::invariant_tsc()`: whether the CPU reports an invariant TSC (CPUID 0x80000007 EDX[8]).
- `LatencyHistogram::negative_count()`: negative samples (stamps from two clocks, or a clock going back) are
  still recorded as 0 but counted; `LatencyRecorder::report()` warns about them.
- `RdtscClock::steady_ns()`: TSC time on a timebase that never jumps; its rate follows `CLOCK_MONOTONIC`
  and any offset from the system clock (after a step) is caught up at most 0.5% faster or slower. Same
  cost as `tscns()` (one `rdtsc`, ~10 ns); kept calibrated by the same `calibrate()` (every second).
- `HttpParseState`: lets `parse_http_response()` resume where the previous call stopped on a growing
  buffer (HttpClient uses it): parsing a chunked body or long headers is no longer quadratic in the reads.
- `WsGroupOptions::dedup_key`: an exchange-native dedup key (e.g. stream + update id) instead of hashing
  the whole text of every copy (~0.4 cycles/byte before the first copy is delivered; ~70 ns flat with a key).
- `WsSessionOptions::max_read_bytes_per_poll`: in busy-poll mode, the most bytes one step reads from a
  session, so a burst on one feed can't hold the loop.
- `FullDuplexShm::try_open()`: opens without throwing (also with `QUARK_NO_EXCEPTIONS`), the reason in an
  optional `std::string*`; for servers opening their clients' files at run time.
- `FullDuplexShm::dropped_on_open()`: the stale messages dropped from the read queue at open (by design).
- `MMapFile::try_initialize()`: the non-throwing form of `initialize()`. Errors now carry their reason.
- `Journal::prefault()` and `Journal::Options::prefault_bytes` (default 4 MiB): the pages ahead of the tail are
  write-faulted (`MADV_POPULATE_WRITE`, Linux 5.14+) by the constructor, `sync()` and `prefault()`, so appends
  no longer page-fault once per 4 KiB page (the file being mlock'ed didn't prevent it). `sync()` now flushes
  only the records.

### Fixed

- `RdtscClock::steady_ns()` froze its rate on a lasting TSC rate change of more than 1% (a sample "too far"
  from the current rate was always rejected) and then drifted from wall time without bound (2%: -11.8 s
  after 10 min). A change confirmed by consecutive samples is adopted, and the offset is always corrected.
- `SlidingWindowLimiter::try_acquire()`: `units` near UINT32_MAX wrapped the "fits" check and were taken;
  `units` of 0 filled the window with empty entries until every acquire failed.
- The `QUARK_ENABLE_LOGGER=OFF` build failed with unused-parameter/-variable errors (LatencyRecorder.h,
  QuarkLog.cpp).
- `HttpClient::request()` returned `Ok` after the connection had failed inside it and `on_error` had
  already run: callers that set "busy" on `Ok` stayed busy forever. Such a failure is now reported by
  `on_error` after `request()` returns (an `Ok` request always gets exactly one later callback).
- A stream of 1xx interim responses overflowed the stack (one recursive call each: SIGSEGV with a 1 MiB
  buffer). They are skipped in a loop, at most `kMaxInterimResponses` (16).
- TLS over OpenSSL: a stale entry in the thread's OpenSSL error queue (from any other OpenSSL call on the
  thread) made a read or write on a healthy connection fail and tear it down. The queue is cleared before
  each SSL call (OpenSSL backend only: AWS-LC was not affected).
- `WsGroup::send_best()` returned WouldBlock when the lowest-RTT session's queue was full although
  another session was open: the next one is tried (only for WouldBlock: a Dropped frame may have reached
  the venue).
- HttpClient: a close-delimited response that exactly filled the receive buffer failed instead of being
  delivered at the close.
- `Journal::append()` accepted an empty payload, which is the journal's end marker: replay stopped there,
  and the next reopen overwrote every later record. Empty appends are now refused.

- Clang builds failed to link the TLS code (undefined `quark::net::detail::openssl_error_string()` and
  `on_new_tls_session()`): two internal headers declared `namespace quark::net::detail` /
  `quark::crypto::detail` outside the versioned `quark::v1`, which GCC reopens as `quark::v1::…` but clang
  doesn't. A test now checks that every `quark::` symbol of the library is in `quark::v1`.
- `ScaleInt` from `double` (constructor, `operator=`, `try_from_double`, `TickSize`) could end one raw unit
  off, depending on the compiler and `-march`: clang fused the scaling multiply and the rounding add into one
  FMA (when folding constants, and at run time with `-march=native`), skipping the product's rounding, e.g. `ScaleInt<13>(500.0000000000001)` gave raw
  5000000000000002 instead of 5000000000000001. The product is now always rounded first, the result GCC in
  ISO mode already gave.

## [1.2.0]

### Added

- `ScaleInt<SCALE, Rep>` / `TickSize<SCALE, Rep>`: the raw value can be an `int128_t` (`Rep = int128_t`,
  SCALE 1..18) for values beyond int64's ~19 significant digits, e.g. crypto prices from 1e-13 to 1e9.
  `Rep` defaults to `int64_t`: existing code is unchanged.
- `ScaleInt::from_chars` / `ScaleInt::parse`: exact decimal text to `ScaleInt` (sign, fraction, exponent),
  without going through double. Text needing more than SCALE fraction digits or not fitting is rejected
  (`errc::result_out_of_range`), never rounded or wrapped.
- `mul()` / `div()`: exact `ScaleInt` x `ScaleInt` and `ScaleInt` / `ScaleInt` at a chosen output scale,
  rounded once with a `Rounding` mode (`HalfAway`, `Floor`, `Ceil`, `TowardZero`); false on overflow or
  division by zero.
- `ScaleInt::try_rescale()`: checked conversion across scales and representations; `TickSize::try_from_value()`,
  `TickSize::value()`.
- `quark/math/Int128.h`: `int128_t`, `uint128_t`, the `Integer` / `SignedInteger` concepts (std's concepts
  reject `__int128` in strict ISO mode), `make_unsigned_t`, `to_chars(uint128_t)`. The `MathOps.h` integer
  helpers accept the 128-bit types.
- `WsSendStatus::Dropped`: `send()` reports a frame lost because the connection failed while it was
  being written (it returned `Ok` before). Frames still queued at a teardown are logged.
- `WsSessionOptions::ping_text` / `pong_text`: application-level pings (e.g. Bitget's `"ping"`), with the
  reply swallowed; `WsSession::last_rtt_ns()`; liveness presets `WsSessionOptions::market_data()`,
  `push_stream()`, `order_entry()`.
- `net::Resolver`: keeps hosts' addresses fresh on its own thread; `EndpointProvider` and
  `set_endpoint_provider()` on `WsSession` / `HttpClient` choose the address of every automatic reconnect.
- `HttpResponseView::headers` and `header(name)` (case-insensitive), e.g. rate-limit headers.
- `ShmDirectory`: discovers the running clients of `FullDuplexShm` sessions in a directory by their
  file locks.
- `Journal`: append-only, memory-mapped record log with CRC-32C, sequence numbers, torn-tail recovery
  and replay; `crc32c()`.
- `quark/utils/Crc32c.h`: `crc32c()` with x86 SSE4.2 (checked at run time), ARMv8 CRC instructions
  when the target has them, and a portable table otherwise; every variant gives the same result.
- `TimeScopedDedup`: first-seen detection of 64-bit keys within a time window, fixed capacity.
- `WsGroup`: N WebSocket sessions to one host over different addresses: first-arrival dedup with per-session
  delay and rotation of the slowest session (market data), or `send_best()` on the lowest-RTT session
  (order entry).
- `HttpPool`: N keep-alive `HttpClient`s to one host over its addresses, sharing a bounded FIFO of requests
  (each goes to the first idle client; waiting requests time out), so one slow response doesn't hold up the
  others.
- `TcpListener`: accepts connections on the `EventLoop` as connected `TcpSocket`s (`TcpSocket::adopt()`);
  kernel sockets and F-Stack.
- `QuarkConfigTable`: tables inside arrays (`[[venue]]`) are values too — `cfg["venue"][0]["name"]`,
  `QuarkConfigValue::size()`, `QuarkConfig::array_size()`; dot paths index arrays (`"venue[1].name"`).
- Rate-limited logging: `QLOG_{DEBUG,INFO,WARN,ERROR}_LIMIT(interval, logger, ...)`,
  `QUARK_LOG_{WARN,ERROR}_LIMIT(interval, ...)` (quill's `_LIMIT` macros, suppressed counts included).
- `LatencyRecorder`: named latency stages over `LatencyHistogram`, logged as percentiles by `report()`.
- `SlidingWindowLimiter` (`quark/utils/RateLimiter.h`): at most N units per sliding window, time passed in,
  resynced to an exchange's reported usage.
- `enum_index()` in `QuarkEnum.h`: an enumerator's position in `enum_values()`, for per-enumerator tables built
  at compile time.

### Changed

- `EventLoop::step()` returns whether it did work (input read, events dispatched, timers fired), so idle
  steps can be spent on housekeeping; `EventLoop::note_work()` lets Pollables and step hooks report work.
  Callers that ignore the result are unchanged.

## [1.1.0]

### Added

- `QuarkEnum.h`: enum reflection wrapping [magic_enum](https://github.com/Neargye/magic_enum) 0.9.8 —
  `enum_name`, `enum_from_name`, `enum_from_value` (validates enums decoded from untrusted bytes),
  `enum_count_v`, `enum_values`. magic_enum is a new PUBLIC dependency, pinned like quill: found installed
  (`EXACT`) or fetched, installed along with quark, and required by `quark-config.cmake` and `quark.pc`.
- `quark::fmtlib::format_to_n`, for allocation-free formatting into a fixed buffer.

### Changed

- `QUARK_USE_STD_FORMAT` is a PUBLIC compile definition (CMake target and pkg-config). It was private, so
  `QuarkFormat.h` resolved to `std::format` inside quark but to quill's bundled fmt in user code, and inline
  headers that format (`FullDuplexShm.h`) were compiled differently on each side (ODR violation) with
  GCC >= 13 / Clang >= 16.

### Deprecated

- The global `Version` constant: use `quark::kVersion`.

### Fixed

- `quark.pc` finds its prefix relative to its own location (`${pcfiledir}`) instead of the configure-time
  `CMAKE_INSTALL_PREFIX`, so it stays valid with `cmake --install --prefix`, `DESTDIR` or a moved install tree.
- The warning about installing the fetched AWS-LC into `/usr` or `/usr/local` is issued when installing, for
  the prefix actually used, instead of on every configure (also a false positive with `cmake --install --prefix`).
- No more AWS-LC `ENABLE_PRE_SONAME_BUILD` deprecation warning on re-configure: AWS-LC warns about its own
  cached option, which quark now clears before fetching it (irrelevant to the static AWS-LC quark builds).
- `QUARK_PKGCONFIG_DIR` is a STRING cache entry like the other install directories: as a PATH, a relative value
  given on the command line was made absolute against the current directory.

## [1.0.0] - 2026-11-04

First release.

### Modules

- **Networking** (`quark::net`): `EventLoop` (busy-poll by default, or epoll) with `TimerQueue`,
  `Poller`, `TcpSocket`, `TlsContext`/`TlsStream` (TLS 1.2+, AES-128-GCM preferred for TLS 1.3,
  session resumption on reconnect), `WsSession` (RFC 6455 client) and `HttpClient` (HTTP/1.1
  keep-alive), plus `Endpoint`, `Url`, `HttpCodec`, `WsFrame`, `WsHandshake`, `BufferPool`/`IOBuffer`,
  `ExpBackoff` and `AliveMonitor`. Kernel sockets by default, or F-Stack (DPDK) with
  `-DQUARK_USE_FSTACK=ON`, chosen at compile time.
- **Crypto** (`quark::crypto`): `HmacSha256` (precomputed key state, no allocation per message),
  `Ed25519Signer` (with a non-throwing `try_sign`), base64 and hex encoding.
- **Logging**: `QuarkLog` over [Quill](https://github.com/odygrd/quill) 13.1.0, configured from TOML;
  quill's backend thread or a manually polled backend (`LogPoller`); per-logger rotating files
  named with the process start time (`logs/<name>_YYYYMMDD_HHMMSS.log`, GMT); uncaught exceptions
  and crash signals are logged and flushed before the process dies (`crash_handler`); the log
  backend keeps `RdtscClock` calibrated against the system clock.
- **Configuration**: `QuarkConfig`, TOML through [toml++](https://github.com/marzer/tomlplusplus).
- **Utilities**: `MemoryArena`, `SimpleObjectPool`, `CircularBuffer`, `CommandQueue`,
  `InplaceFunction`, `LatencyHistogram`, `RdtscClock` ([tscns](https://github.com/MengRao/tscns)),
  `JsonParser` ([simdjson](https://github.com/simdjson/simdjson)), `DateTime` (trading-day
  arithmetic; `TimestampFormat`, checked once and parsed with a `noexcept`, allocation-free
  `parse()`, plus `parse_timestamp`/`try_parse_timestamp`: strptime-style UTC timestamps with
  nanosecond fractions and UTC offsets), `MMapFile`, `FullDuplexShm`, `HwEndian`,
  `System`, and the SPSC lock-free queues.
- **Math**: `ScaleInt` fixed-point numbers with tick-size rounding, `DoubleOps`, `MathOps`.

### Error handling

- Startup and configuration errors throw `QuarkError`; errors caused by input while running have
  non-throwing APIs (`TimestampFormat::parse`, `try_parse_timestamp`, `Ed25519Signer::try_sign`),
  and WebSocket key generation survives a failing `getrandom()`.
- `-DQUARK_NO_EXCEPTIONS=ON` turns a `QuarkError` into `quark::fatal_error()`: stderr, the
  `set_fatal_handler()` hook, a CRITICAL log line and a bounded flush, then `std::abort()`. The
  whole test suite runs in this mode too (death tests in place of exceptions).
- The public headers compile with `-fno-exceptions` and with `QUARK_ENABLE_LOGGER=OFF`
  (`CircularBuffer`, `WsSession` fixed); tested in 10 configurations: Release, Debug + ASan/UBSan,
  TSan, `QUARK_NO_EXCEPTIONS` (also with ASan), shared, OpenSSL, logger off, F-Stack, benchmarks.

### Build and packaging

- CMake >= 3.16, C++20, Linux x86-64; tested with GCC 11 and 12.
- Few dependencies: no Abseil or Boost; applications choose their own hash and string libraries.
- `-DQUARK_TLS_BACKEND=AWS-LC` (default) or `OpenSSL` (>= 3.0) for crypto and TLS; AWS-LC is found
  installed or fetched and linked statically. Measurements behind the default are in
  [PLAN.md](PLAN.md) ("TLS backend").
- Static (default) or shared library (`-DBUILD_SHARED_LIBS=ON`, soname `libquark.so.1`, only
  `QUARK_API` symbols exported); symbols live in the inline namespace `quark::v1`; `find_package(quark)` CMake package and `quark.pc` for pkg-config.
- Build options for exceptions (`QUARK_NO_EXCEPTIONS`), assertions, sanitizers, coverage, and the
  logging queue (type, capacity, huge pages).

### Dependencies (pinned)

| Dependency | Version | Linked |
|---|---|---|
| simdjson | 5.0.3 | public (`JsonParser.h`) |
| toml++ | 3.4.0 | private, header-only |
| Quill | 13.1.0 (exact; every module of a process must use the same) | public |
| AWS-LC | 5.12.0, or OpenSSL >= 3.0 | private |
| GoogleTest | 1.18.0 | tests only |
| F-Stack / DPDK | 2.0 (+ [patch](doc/f-stack-2.0-eal-argv-double-free.patch), merged upstream in `dev`) / 24.11.8 | optional |

### Known limitations

- `WsSession`: no `permessage-deflate`, no UTF-8 validation of text frames.
- `HttpClient`: one request in flight, response must fit one buffer, no streaming.
- `Endpoint`: blocking DNS (`getaddrinfo`), IPv4 only.
- TLS: no kTLS, no OCSP/CRL checks.
- F-Stack: one event loop per process (`thread_mode=1` is not used); F-Stack benchmarks pending.
- Logging: `max_backup_files` applies per run; files of earlier runs are not removed.

Details: [PLAN.md](PLAN.md), "Known gaps in what exists".

[1.0.0]: https://github.com/Frodocz/quark/releases/tag/v1.0.0
