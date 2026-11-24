# build checklist (build / API / ODR / consumer integration)

Scope: `CMakeLists.txt`, `cmake/*`, `include/quark/{Attributes,QuarkConfig,QuarkError,QuarkFormat,QuarkLog,QuarkEnum,Sanitizers}.h`,
`src/QuarkConfig.cpp`, `src/QuarkLog.cpp`, and how `/home/ubuntu/personal/trading/CMakeLists.txt` consumes quark.

## Suspicions to verify (from reading CMakeLists.txt on 2026-11-24; not confirmed problems)
Verify each one and give a verdict.
1. `QUARK_ENABLE_ASSERTIONS` adds a PRIVATE define that **no source file reads**. Header asserts follow
   the consumer's `NDEBUG`, so the option is a no-op. Also: an inline function whose body differs with
   `NDEBUG` across TUs (a mixed Debug/Release link) is technically an ODR violation. Decide: remove
   the option, or make it drive a `QUARK_DEBUG_ASSERT` macro exposed as PUBLIC.
2. Warnings: the library gets PRIVATE `-Wall -Wextra -Wpedantic`, with no `-Wconversion`/`-Wshadow`,
   and header-only code is checked only in quark's own tests (`-Werror`). Suggest a
   "compile each public header alone (self-contained) + strict warnings" target.
3. `QUARK_NO_EXCEPTIONS`: the define is PUBLIC, `-fno-exceptions` is PRIVATE. Trading builds with
   exceptions on and catches `simdjson_error`. Does every test pass in both configs? Does any inline
   header behave differently between quark's TUs and the consumer's?
4. `std::format` selection is by compiler version, not the standard library (Clang ≥ 16 with
   libstdc++ < 13?). The comment says "GCC 13.1" but the check uses 13.0. The dev box (GCC 11.4) covers only the fmt path.
5. Build-script hygiene: `project()` is called twice; the `if (NOT CMAKE_CXX_STANDARD)` branch after
   setting it is dead code; the log prints the undefined `QUARK_BUILD_MODULE`; `QUARK_SANITIZE_*`
   says "in tests" but changes the global `CMAKE_CXX_FLAGS` (and so the parent project's flags when
   quark is added with add_subdirectory from trading).
6. Dependencies are fetched over the network at configure time with tags pinned but no hashes:
   reproducibility and supply-chain risk for production builds.
7. Logging: BoundedDropping loses logs under pressure (see runtime.md). The PUBLIC defines
   `QUARK_LOG_QUEUE_TYPE`/`CAPACITY`/`HUGE_PAGES` must match across modules.
8. No optimisation or arch flags are set by quark or trading (`-march`, LTO, `-fno-plt`). Report the
   flags that actually apply from `compile_commands.json` and assess their effect on the hot path
   (e.g. crc32/AES/SHA instructions, simdjson's runtime dispatch).
9. Consumer integration: trading uses `add_subdirectory` with `QUARK_SOURCE_DIR`, so quark's
   global-scope settings (CMAKE_CXX_FLAGS changes, `FORCE`d cache variables like `SIMDJSON_INSTALL`,
   `QUARK_MASTER_PROJECT`) leak into trading. List each one.

## Option matrix (at least "builds + tests pass")
Default / `QUARK_NO_EXCEPTIONS=ON` / `QUARK_ENABLE_LOGGER=OFF` / `QUARK_TLS_BACKEND=OpenSSL` /
`BUILD_SHARED_LIBS=ON` / ASan+UBSan / TSan.
Run `${CLAUDE_SKILL_DIR}/scripts/build_matrix.sh rel noexc nologger openssl shared asan` and list the failures.
`QUARK_USE_STD_FORMAT` is an internal CMake variable (not a cache option); covering both cases needs
a different compiler or standard library (GCC ≥ 13 or Clang ≥ 16).

## Public API surface
- Version consistency and ODR protection for types exposed PUBLIC (simdjson, quill, magic_enum).
- What `QUARK_API` exports; the rule linking SOVERSION/`inline namespace v1` to ABI changes.
- Whether inline functions in headers produce duplicate symbols in shared builds because of visibility settings.
- Headers that leak generic names or macros into consumer code (LL-004).
