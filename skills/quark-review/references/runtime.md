# runtime checklist (QuarkLog / QuarkConfig / QuarkError / QuarkEnum / QuarkFormat / System)

Scope: `QuarkLog.h` + `src/QuarkLog.cpp`, `QuarkConfig.h` + `src/QuarkConfig.cpp`, `QuarkError.h`
(fatal path), `QuarkEnum.h`, `QuarkFormat.h`, `utils/System.h` + `src/utils/System.cpp`, `Sanitizers.h`.
Consumers: every trading binary uses `quark::ScopedLog`, the `QUARK_LOG_INFO/WARN/ERROR` macros
(no `_LIMIT` variants; the parsers rate-limit by hand) and `quark::Frontend::get_logger`;
`QuarkConfig::load_file`/`get_or`/`table_names`/`QuarkConfigArray`; `enum_name`/`enum_values`/`enum_index`
in tradecommon; `pin_thread_to_cpu`, `install_shutdown_handler`/`shutdown_requested`.

## Checks
1. Log producer (hot): cost per `QUARK_LOG_*` on the caller thread (encoding only, no formatting);
   arguments that allocate or copy (std::string by value, user types without a cheap codec);
   behaviour when the queue is full under BoundedDropping (counted? reported?); does any logic depend
   on a log being delivered? Memory per thread at 2 MiB × threads; when huge pages fall back to
   normal pages, is the queue pre-faulted (otherwise page-fault jitter on first use)?
2. Log backend: it also drives `RdtscClock::calibrate()`, so what happens to tscns accuracy if the
   backend stalls or logging isn't initialised? Shutdown order (ScopedLog destruction vs threads
   still logging); flush on crash (`crash_handler`, `fatal_flush_timeout`); signal-safety of the
   crash path; quill version pinning (static_assert in QuarkLog.cpp); whether raw quill APIs work on quark's loggers
   (trading had to use `quark::Frontend::get_logger`, because `quill::Frontend::get_logger` aborts on them).
3. Fatal path: `fatal_error()` re-entrancy, whether the user's handler can deadlock (it runs before
   the flush), stderr ordering, and `QUARK_NO_EXCEPTIONS` vs exceptions-on behaviour of the same call site.
4. QuarkConfig (cold, but a bad config means a wrong trade): type coercion (int vs float vs string),
   missing vs default (`get_or` silently hiding typos?), errors that name the key path, arrays of
   tables, secrets handling, nothing kept that points into a toml document that has been freed.
5. QuarkEnum: magic_enum range limits (`MAGIC_ENUM_RANGE_MIN/MAX`) vs trading enum values (out of
   range → empty name / wrong index); compile-time `enum_index` correctness; whether a mismatch
   between the magic_enum version and range macros in two TUs is an ODR risk.
6. QuarkFormat: std::format vs fmt selection must be identical in every TU (PUBLIC define); no
   formatting on hot paths.
7. System: `pin_thread_to_cpu` error handling and the isolated-core warning; `set_thread_realtime`
   and RT throttling (`doc/TUNING.md`); shutdown handler async-signal-safety (only `sig_atomic_t`/lock-free atomics).

## Verification
- `strace -f` / `perf stat` on a logging benchmark (`benchmarks/utils/UtilsBench.cpp`), calls on the hot thread only.
- AllocGuard around `QUARK_LOG_INFO` with typical trading argument types.
- Tests: queue-full drop counting, crash flush (`tests/core/FatalTest.cpp`), config typo/missing-key errors.
