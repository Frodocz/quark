# quark project context

> `CMakeLists.txt` and `cmake/` are the source of truth; this file is a map for orientation.
> Facts marked **(verified YYYY-MM-DD)** were checked against code on that date. Re-check anything a
> finding depends on. Items marked **(assumption)** still need the owner's confirmation. Update
> this file when triage or the user corrects it.

## Build and options (verified 2026-11-24)
- C++20, `CMAKE_CXX_EXTENSIONS=OFF`, GCC/Clang, default build type Release (`-O3 -DNDEBUG`). No
  `-march`, LTO or PGO is set by quark or by trading.
- Options: `QUARK_NO_EXCEPTIONS` (OFF), `QUARK_ENABLE_ASSERTIONS` (OFF), `QUARK_ENABLE_LOGGER` (ON),
  `QUARK_USE_FSTACK` (OFF), `QUARK_TLS_BACKEND` (AWS-LC default, or OpenSSL ≥ 3.0),
  `QUARK_LOG_HUGE_PAGES` (ON), `QUARK_LOG_QUEUE_TYPE` (BoundedDropping, so logs are dropped when the
  queue is full), `QUARK_LOG_INITIAL_QUEUE_CAPACITY` (2 MiB per thread), `BUILD_SHARED_LIBS` (OFF),
  `QUARK_SANITIZE_ADDRESS` (ASan+UBSan), `QUARK_SANITIZE_THREAD`, `QUARK_CODE_COVERAGE` (-O0),
  `QUARK_BUILD_BENCHMARKS` (OFF), `QUARK_BUILD_TESTS`/`EXAMPLES` (ON).
- **`QUARK_ENABLE_ASSERTIONS` is defined (PRIVATE) but referenced by no source file.** The
  `assert()` calls in headers (MathOps, SimpleObjectPool, CircularBuffer, TimerQueue, IOBuffer,
  MemoryArena, RateLimiter, TimeScopedDedup) depend only on the **including TU's** `NDEBUG`.
  `QUARK_ASSERT` / `QUARK_REQUIRE` (QuarkError.h) are always on.
- Dependencies: quill 13.1.0 (PUBLIC), simdjson v5.0.3 (PUBLIC, marked SYSTEM), magic_enum 0.9.8
  (PUBLIC, SYSTEM), tomlplusplus v3.4.0 (PRIVATE), AWS-LC 5.12.0 or OpenSSL (PRIVATE), Threads.
  They are fetched with FetchContent at configure time (git tags, no hashes); quill and magic_enum
  reuse a parent's or the system's copy if the version matches exactly.
- `std::format` is used when GCC ≥ 13.0 or Clang ≥ 16 (decided by compiler version, not by the
  standard library), otherwise quill's bundled fmt. `QUARK_USE_STD_FORMAT` is PUBLIC.
- Test/bench/example targets use `quark_set_common_compile_options`: `-Wall -Wextra -pedantic
  -Werror -Wredundant-decls -Wfloat-equal`. The library itself gets only `-Wall -Wextra -Wpedantic`.
- Much of the code is header-only and gets instantiated in the consumer's TUs. Watch ODR and that
  macros and flags agree between the library and its consumers.
- Inline namespace `quark::v1`; the SOVERSION is the major version; visibility is hidden except for
  `QUARK_API`.

## Toolchain on the dev box (verified 2026-11-24; re-check with `command -v`)
- Present: GCC 11.4 (so `std::format` is off and quill's fmt is used), cmake, ctest, ninja, objdump,
  strace, perf, python3.
- Missing: clang/clang++ (so TSan falls back to GCC, which can report false positives on
  `atomic_thread_fence`), clang-tidy, cppcheck, gcovr, lcov, ltrace, heaptrack, valgrind.
  Install: `pip install gcovr`, `sudo apt install clang clang-tidy cppcheck lcov valgrind heaptrack`.
- Existing build dirs use Unix Makefiles.

## Modules
- **math**: ScaleInt, Int128, DoubleOps, MathOps
- **crypto**: Ed25519Signer, HmacSha256, Base64Encoding
- **net**: EventLoop, Poller, SysApi, TcpSocket, TcpListener, Endpoint, Resolver (runs its own
  DNS-refresh thread), Stream, TlsContext/TlsStream, HttpClient/HttpPool/HttpCodec,
  WsFrame/WsHandshake/WsSession/WsGroup, AliveMonitor, ExpBackoff, BufferPool, IOBuffer, Url,
  FStack (optional)
- **utils**: RdtscClock, DateTime, JsonParser, TimerQueue, CircularBuffer, CommandQueue,
  FullDuplexShm, ShmDirectory, Journal, MemoryArena, SimpleObjectPool, InplaceFunction, MMapFile,
  RateLimiter, TimeScopedDedup, LatencyRecorder, LatencyHistogram, Crc32c, HwEndian, System
- **top level**: Attributes, QuarkConfig, QuarkEnum, QuarkError, QuarkFormat, QuarkLog, Sanitizers
- **third_party (vendored)**: inplace_function, SPSC_Queue (SPSCQueue/OPT, SPSCVarQueue/OPT), tscns
- Design docs at the repo root: `REWRITE_PLAN.md`, `GAPS_PLAN.md` (why features exist, and the
  thread layout), `doc/TUNING.md` (host tuning), `doc/FSTACK.md`.

## How trading uses quark (verified 2026-11-24 against /home/ubuntu/personal/trading)
- Integration: trading's `CMakeLists.txt` does `add_subdirectory(${QUARK_SOURCE_DIR})` (default
  `../quark`, with quark tests/examples OFF) or falls back to `find_package(quark 1.2)`. It sets **no**
  quark options, so production uses the defaults: exceptions ON, AWS-LC, logger ON, BoundedDropping,
  Release with `NDEBUG`. Trading's own targets use `-Werror`.
- Two processes, connected only by shared memory:
  - **alphaframe** (strategy + market data): one pinned busy-poll `EventLoop` thread runs the MD
    WebSockets, REST snapshots, the strategy, timers, and the `TsClient` SHM poll.
  - **tradeserver**: one pinned busy-poll `EventLoop` thread (SHM sessions → OMS → Router → venue
    WS-API/REST) plus a **SessionThread** that, every 200 ms, runs `ShmDirectory::alive()`, opens and
    unmaps `FullDuplexShm` sessions, and calls `Journal::sync()` under a mutex while the loop thread
    appends. Sessions are handed over through mutex-protected vectors.
  - Threads started by quark: the quill backend (which also calls `RdtscClock::calibrate()`) and the
    `Resolver` refresh thread.
  - **backtest**: single-threaded, simulated time, in-process link instead of SHM. **tradecommon**:
    QuarkEnum and JsonParser (symbol table).
- **Cross-process ABI**: `FullDuplexShm<64 MiB>` over `SPSCVarQueue` must stay byte-compatible with
  the legacy binaries (file = 64 MiB + 256 B; see `TestShmLayout`). The `Journal` record format is
  persisted across restarts. Changing either is a breaking change.
- Trading catches `simdjson::simdjson_error` around `JsonParser` in every per-message parser and
  catches `std::exception` from `FullDuplexShm`/config construction. It is **not** compatible with
  `QUARK_NO_EXCEPTIONS=ON` as written.
- Exchanges: Binance USDⓈ-M (WS-API orders, HMAC per request) and Bitget UTA (HMAC only at login/REST).

### Consumer map (what trading runs, and where)

| quark component | Trading use | Class |
|---|---|---|
| WsSession / WsGroup (+TimeScopedDedup) / TlsStream / BufferPool | MD feeds (`alphaframe/src/md/Feed.cpp`), order send `WsGroup::send_best` (`tradeserver/src/BinanceUsdm.cpp`) | **hot, per message** |
| JsonParser (`parse_in_place`, prices as `double`) | MD parsers (`alphaframe/src/md/BinanceUsdm.cpp`, `BitgetUta.cpp`), order acks/fills (`tradeserver/.../BinanceUsdm.h` `EventParser`) | **hot, per message** |
| HmacSha256 (`sign_hex`) | Every Binance order/cancel/modify (`tradeserver/src/BinanceUsdm.cpp`, `WsApiSigner`) | **hot, per order**; dominates the 2 µs `Router::insert` |
| RdtscClock::tscns, LatencyRecorder | Stamping every message/order (`alphaframe/src/st/Engine.cpp`, `tradeserver/src/TradeServer.cpp`) | **hot** |
| FullDuplexShm (`try_write`/read), SPSCVarQueue | alphaframe `TsClient` ↔ tradeserver `AlgoSession` | **hot, cross-process** |
| EventLoop / TimerQueue (via `timers()`) / Poller | Both processes' main loop | **hot** |
| SlidingWindowLimiter (RateLimiter.h) | `Router::insert` | **hot, per order** |
| SimpleObjectPool<Order>, InplaceFunction (32 B) | alphaframe `OrderManager.h`, `Order.h` | **hot** |
| Journal (append) | tradeserver loop thread; `sync()` and rotation elsewhere | **hot** (append) / warm (sync, recover) |
| HttpClient<TlsStream> | REST snapshots, REST queries | warm |
| ShmDirectory, Resolver, TlsContext, Url | Session discovery, DNS, setup | cold |
| QuarkLog (`ScopedLog`, INFO/WARN/ERROR macros, `quark::Frontend`) | All binaries | producer side hot, backend cold |
| QuarkConfig (`load_file`, `get_or`, `QuarkConfigArray`), QuarkEnum, System (`pin_thread_to_cpu`, shutdown handler) | All binaries | cold (enum_name may be hot when used in logs) |
| DateTime (`Date`, `TradingDay`) | alphaframe `Strategy.h` | warm |
| **Not used by trading:** ScaleInt and all of math, Ed25519Signer, Base64Encoding, HttpPool, TcpListener/TcpSocket (direct), F-Stack, CommandQueue, CircularBuffer, MemoryArena, HwEndian, standalone SPSCQueue | — | latent: review for correctness, mark severity "latent" |

Refresh this map with:
`grep -rhoE '#include [<"]quark/[^>"]+' /home/ubuntu/personal/trading --include=*.h --include=*.cpp --exclude-dir='build*' | sort | uniq -c | sort -rn`

## Hot-path map (starting assumption; verify)

| Module / file | Class | Notes |
|---|---|---|
| math/* | hot (no consumer yet) | price/qty/accounting arithmetic once trading moves off `double` (assumption) |
| RdtscClock, tscns | hot | `tscns()` goes through a function-local static (guard check per call); calibrated from the quill backend thread |
| LatencyRecorder / LatencyHistogram | hot record path; reporting cold | |
| SPSC queues, FullDuplexShm, CommandQueue, CircularBuffer | hot | |
| TimerQueue | hot | polled every loop iteration; vector heap + chunked nodes, `reserve()` up front |
| MemoryArena, SimpleObjectPool, BufferPool, IOBuffer | hot | |
| JsonParser | hot | |
| EventLoop, Poller, TcpSocket, TlsStream, WsSession, WsGroup, WsFrame, HttpCodec | hot | |
| HmacSha256 | hot | per Binance order |
| Ed25519Signer | warm (no consumer yet) | hot if a venue signs each order with it |
| HttpClient, HttpPool | warm | |
| QuarkLog (producer enqueue side) | hot | formatting and writing happen on the backend thread |
| Journal | append hot, sync/scan warm | |
| Resolver, TlsContext, WsHandshake, Endpoint, AliveMonitor, ExpBackoff | cold | connection setup and maintenance |
| QuarkConfig (toml), MMapFile, ShmDirectory, System, DateTime parsing at start-up | cold | |

## Error-handling policy (verified 2026-11-24, from QuarkError.h)
- Errors quark can't return (constructors, configuration) are reported by throwing `QuarkError`
  via `QUARK_THROW`. With `QUARK_NO_EXCEPTIONS`, `QUARK_THROW`/`QUARK_REQUIRE` call `fatal_error()`
  instead: stderr, then the user's `set_fatal_handler` hook, then a CRITICAL log + flush bounded by
  `fatal_flush_timeout`, then `abort()`. `QUARK_CATCH` blocks become dead code.
- Errors caused by external input at runtime (malformed timestamp, failed signature, bad JSON)
  must have non-throwing APIs: `try_parse_timestamp`, `TimestampFormat::parse`, `try_sign`,
  `try_from_double`, `try_rescale`, `try_read`/`try_write`, `try_acquire`. JsonParser surfaces
  simdjson errors, and trading catches `simdjson_error`.
- `QUARK_ASSERT` is checked in every build type. Plain `assert()` is debug-only.
- Users of `QUARK_THROW`: JsonParser, InplaceFunction, Journal, FullDuplexShm, MemoryArena,
  CircularBuffer, EventLoop, BufferPool, and the .cpp files for crypto, MMapFile, QuarkLog, DateTime, TlsContext.

## Open questions for the owner
1. Production target CPU and whether trading should set `-march`/LTO (not set today).
2. Will trading move prices/qty to ScaleInt? (Decides whether `math` findings are latent or Critical.)
3. Is `QUARK_NO_EXCEPTIONS` a supported production mode, or only a tested option?
4. Is the hot/warm/cold map above right?
