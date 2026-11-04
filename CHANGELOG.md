# Changelog

All notable changes to quark are listed here. quark follows [semantic versioning](https://semver.org):
the major version changes with incompatible API or ABI changes, so `find_package(quark 1.0)` accepts
any 1.x and `libquark.so.1` stays binary compatible across 1.x releases.

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
