# quark project context

> `CMakeLists.txt` and `cmake/` are the source of truth; this file is a map for orientation.
> Facts marked **(verified YYYY-MM-DD)** were checked against code on that date; **(owner YYYY-MM-DD)**
> marks a decision the owner stated. Re-check anything a finding depends on. Update this file when
> triage or the user corrects it.

## Build and options (verified 2026-11-24)
- C++20, `CMAKE_CXX_EXTENSIONS=OFF`, GCC/Clang, default build type Release (`-O3 -DNDEBUG`). quark
  sets no `-march`, LTO or PGO. Trading doesn't either *yet*, but it is to build with
  `-march=native` + LTO (see "Production target").
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

## Production target (owner 2026-11-25: "use the dev machine as the target"; verified 2026-11-25)
- Provisional: production hardware isn't decided, so **this dev box is the reference target**.
  Re-measure if production moves.
- AWS **m6a.2xlarge**, KVM guest, **AMD EPYC 7R13 (Zen 3; `-march=native` = `znver3`)**, 8 vCPUs =
  4 cores × SMT2, 1 socket, 1 NUMA node; L1d 32 KiB/core, L2 512 KiB/core, L3 16 MiB shared.
  Kernel 6.8.0-1069-aws.
- TSC: `constant_tsc nonstop_tsc tsc_known_freq rdtscp`, clocksource `tsc`. The TSC is virtualised
  by KVM, so treat calibration accuracy and drift as things to measure, not assume.
- ISA available: SSE4.2 (CRC32C), AVX2, BMI2, AES-NI, VAES, VPCLMULQDQ, **SHA-NI** (SHA-256 for
  HMAC). **No AVX-512.**
- No isolated cores (`/sys/devices/system/cpu/isolated` is empty). The `doc/TUNING.md` settings
  aren't applied, so benchmark tails here include scheduler and IRQ noise. Pin benchmarks to one
  core and keep its SMT sibling idle.
- **Trading build (owner 2026-11-25): `-march=native` + LTO.** Effects on reviews:
  - Benchmark and hot-path numbers count only when built with `-march=native` and LTO (the
    `build_matrix.sh rel` config has neither, so call such numbers "generic x86-64").
  - With `-march=native`, ISA paths are chosen at compile time (`__SSE4_2__`, `__SHA__`, `__AVX2__`).
    Check that both the native and the generic paths are tested and agree (CRC32C,
    HMAC/SHA-256 in AWS-LC, simdjson kernels).
  - LTO inlines across quark's `.cpp` files and trading, and exposes ODR violations (`-Wodr`, which
    GCC reports only with LTO). Any `NDEBUG`- or macro-dependent inline body that differs between
    TUs becomes a real risk.
  - The binary isn't portable to another CPU: a production host that isn't Zen 3 or newer must be rebuilt.

## Toolchain on the dev box (verified 2026-11-25; re-check with `command -v`)
- Present: GCC 11.4 (so `std::format` is off and quill's fmt is used), **clang/clang++ 14.0**
  (Ubuntu; TSan + libFuzzer runtimes; uses libstdc++ 11), **gcovr 8.8** (`~/.local/bin`), cmake,
  ctest, ninja, objdump, strace, perf, python3.
- Missing: clang-tidy, cppcheck, lcov, ltrace, heaptrack, valgrind
  (`sudo apt install clang-tidy cppcheck lcov valgrind heaptrack ltrace`).
- Clang 14 is below the Clang ≥ 16 `std::format` cut-off, so both compilers build only the fmt
  path. Covering `std::format` needs GCC ≥ 13 or Clang ≥ 16.
- The older build dirs (`build/`, `build-*`) use Unix Makefiles; `build/review/*` use Ninja.
- **Mixed GCC versions by default (verified 2026-11-25).** `cc`/`gcc` → **gcc-12** (an alternative set
  in 2026-06), while `c++`/`g++` → **g++-11**; clang 14 also selects GCC 12's libstdc++ headers. With
  CMake's defaults, AWS-LC (C) builds with gcc-12, and C++ targets link with `-L/usr/lib/gcc/x86_64-linux-gnu/12`.
  Consequences: coverage records nothing (libgcov 12 vs objects from GCC 11), and **LTO across the two
  versions can't work** (GCC 11 can't read GCC 12 LTO bytecode), which matters for trading's planned
  LTO build. The review scripts pin the matching C compiler (`gcc-11` for `g++-11`, `clang` for `clang++`).
  Trading's own build has the same mix until the owner pins `CC`/`CXX`.
- **quark's tests don't build with clang (verified 2026-11-25).** `cmake/QuarkUtils.cmake:61` adds
  `-Wimplicit-int-float-conversion` for Clang to the `-Werror` test/bench/example targets, and vendored
  `tscns.h` (included by `RdtscClock.h`, not a system header) triggers it 5 times. Trading's flags
  don't enable that warning, so trading isn't affected. Workaround for clang runs:
  `CMAKE_ARGS_EXTRA="-DCMAKE_CXX_FLAGS=-Wno-error=implicit-int-float-conversion"`.

## Baseline from triage (verified 2026-11-24, quark cd6855c)
- rel / asan / tsan(GCC): 504 / 508 / 505 tests, all passing, no sanitizer reports. The counts
  differ because of `NDEBUG`-only and ASan-only tests, which is by design.
- All 56 public headers are self-contained (`scripts/header_selfcontained.sh`).
- Vendored SPSC_Queue, inplace_function and tscns (de26b45) are byte-identical to upstream.
  `third_party/tscns/.git/` is a nested clone on disk only, not tracked.
- Strict warnings (`warn`): 199 `-Werror` errors in quark code; 152 of them in vendored `tscns.h`.
- Coverage (2026-11-25, gcovr, GCC 11 Debug, 505 tests): **85% lines (5953/6939), 57% branches
  (4841/8418)** for include/quark + src, excluding third_party. Per-file: `build/review/cov/coverage.lines.txt`.
- clang 14 TSan (2026-11-25): 395 of 400 tests built and ran, **0 TSan reports**. Not built:
  - TLS-linked tests (tls_stream, ws_session, http_client, binance_ws example): the namespace link
    bug in `src/net/TlsInternal.h` (below).
  - timer_queue: `TimerQueueTest.cpp` replaces global `operator new`, which clashes with clang's TSan runtime.
  - 1 failure: `TestScaleInt.TestToRawMatchesLlround` (FMA contraction, below).
- Known clang-only defects (found in triage, not yet fixed or reviewed):
  - `src/net/TlsInternal.h` / `src/crypto/OpenSslError.h` declare `namespace quark::net::detail` /
    `quark::crypto::detail` without `QUARK_BEGIN_NAMESPACE`. GCC treats that as extending `quark::v1::…`
    (C++20 inline-namespace rule); clang 14 creates a separate namespace, so the TLS code fails to link.
  - `ScaleInt::to_raw` (`math/ScaleInt.h:219-223`, `v * SCALE + 0.49999999999999994`) gives
    compiler/flag-dependent results: clang contracts it into an FMA (when constant-folding, and at runtime
    with `-march=native`), e.g. `ScaleInt<13>(500.0000000000001)` gives raw …002 instead of …001. GCC
    in ISO mode, the planned trading build, is unaffected.

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
- Trading today catches `simdjson::simdjson_error` around `JsonParser` in every per-message parser
  (simdjson's throwing `.value()` style, e.g. `alphaframe/src/md/BinanceUsdm.cpp:47-63`) and catches
  `std::exception` from `FullDuplexShm`/config construction (`tradeserver/src/TradeServer.cpp:221`,
  opening a client's SHM session). It is **not** compatible with `QUARK_NO_EXCEPTIONS=ON` as written;
  see "Exception policy target" for the planned move.
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
| **Planned (owner 2026-11-25), not used yet:** ScaleInt for Price/Qty | — | latent-planned: rate by the impact it will have once adopted, label "planned consumer" |
| **Not used by trading:** the rest of math (except as ScaleInt dependencies), Ed25519Signer, Base64Encoding, HttpPool, TcpListener/TcpSocket (direct), F-Stack, CommandQueue, CircularBuffer, MemoryArena, HwEndian, standalone SPSCQueue | — | latent: review for correctness, mark severity "latent" |

Refresh this map with:
`grep -rhoE '#include [<"]quark/[^>"]+' /home/ubuntu/personal/trading --include=*.h --include=*.cpp --exclude-dir='build*' | sort | uniq -c | sort -rn`

## Hot-path map (confirmed by the owner 2026-11-25; still flag code that contradicts it)

| Module / file | Class | Notes |
|---|---|---|
| math/* | hot (planned consumer) | price/qty arithmetic once trading moves Price/Qty to ScaleInt (owner 2026-11-25) |
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

## Exception policy target (owner 2026-11-25)
- Intent: run production with **`QUARK_NO_EXCEPTIONS=ON`**, working like `QUILL_NO_EXCEPTIONS`.
  The mechanism already matches quill (`QUILL_TRY/THROW/CATCH` → `QUILL_REQUIRE` → abort;
  `QUARK_THROW` → `fatal_error()` → handler, flush, abort). Reason given: lower latency.
- Reviewer position (challenge raised 2026-11-25; the owner keeps the decision). Exceptions cost
  nothing on the non-throwing path (table-based unwinding). The latency comes from **throwing**:
  a throw costs µs and takes the unwinder's locks. Today that happens per malformed message
  through simdjson's `.value()`, not through `QUARK_THROW`. So:
  1. The latency win comes from parsing with error codes (`.get(x)` / `error_code`) on the hot path. That's
     independent of the flag, and needs `-fno-exceptions` *in trading's TUs* (today the flag is
     PRIVATE to quark's `.cpp` files) for simdjson to switch to `SIMDJSON_EXCEPTIONS=0`.
  2. With the flag ON, every `QUARK_THROW` becomes an abort. Errors from **runtime external input**
     must first get non-throwing APIs, e.g. a client's SHM file failing `flock`/size checks in
     `FullDuplexShm` (today caught at `TradeServer.cpp:221`, under NO_EXCEPTIONS it would abort the
     tradeserver with orders open), `MMapFile`, `Journal` open.
  3. Measure before switching: the `rel` vs `noexc` benchmarks on the target.
- Review rules that follow:
  - A `QUARK_THROW`/`QUARK_REQUIRE` reachable from **runtime external input** (peer SHM files,
    network data, exchange responses) is **Major**: it aborts in the target production mode.
  - Configuration and start-up throws are fine (an abort at start-up is the intended behaviour).
  - Hot-path code must not throw in either mode; check with `hotpath_symbols.sh` for `__cxa_throw`.
  - `noexc` is a required config in every `build_matrix.sh` run that touches headers or error handling.

## Open questions for the owner
1. Production hardware isn't decided. Re-run the target section when it is, especially if it isn't
   AMD Zen 3 (the `-march=native` binary won't run elsewhere).
