---
name: quark-review
description: Read-only C++20 code review of quark, the low-latency crypto HFT base library used by alphaframe/tradeserver/backtest in /home/ubuntu/personal/trading. Covers math/ScaleInt, rdtsc/time, net (EventLoop/TLS/WebSocket/WsGroup/HTTP), SPSC/shm/Journal/pools/TimerQueue, JSON/crypto parsing, logging/config/runtime, CMake/ODR. Use when the user asks to review, audit, triage or assess quark for correctness, tail latency, hot-path allocation, UB, data races or test/benchmark gaps.
argument-hint: "triage | diff [git-ref] | math | time | net | containers | parsing | runtime | build"
disable-model-invocation: true
---

# quark-review (read-only review)

Review quark, an ultra-low-latency C++20 base library consumed by the trading modules in
`/home/ubuntu/personal/trading` (alphaframe = strategy/market-data process, tradeserver = order gateway
process, backtest, tradecommon). This skill **only reviews; it never modifies files in the repo**
(quark or trading). Fixes go through `/quark-fix`.

Arguments typed after the command: $ARGUMENTS

`${CLAUDE_SKILL_DIR}` is this skill's directory: scripts are in `${CLAUDE_SKILL_DIR}/scripts/`,
references in `${CLAUDE_SKILL_DIR}/references/`, templates in `${CLAUDE_SKILL_DIR}/templates/`.
Run commands from the quark repo root.

## 1. Route first

Pick the checklist from the argument and read **only** that one (keeps context small). With no
argument, ask which scope to review and show this table.

| Argument | Scope | Checklist |
|----------|-------|-----------|
| `triage` | Whole-library read-only triage: module table + deep-review order | section 7 of this file |
| `diff [ref]` | Only what changed since `ref` (default: `main`, or `HEAD~1` when on main), mapped to areas | section 8 of this file + the checklists of the touched areas |
| `math` | ScaleInt / Int128 / DoubleOps / MathOps | references/math.md |
| `time` | RdtscClock / tscns / DateTime / Latency* / RateLimiter / TimeScopedDedup | references/time.md |
| `net` | EventLoop / Poller / Tcp / Tls / Http / WebSocket / WsGroup / Buffer / F-Stack | references/net.md |
| `containers` | SPSC queues / FullDuplexShm / ShmDirectory / Journal / pools / arena / TimerQueue / InplaceFunction | references/containers.md |
| `parsing` | JsonParser / crypto / Crc32c / HwEndian / Url | references/parsing-crypto.md |
| `runtime` | QuarkLog / QuarkConfig / QuarkError + fatal path / QuarkEnum / QuarkFormat / System | references/runtime.md |
| `build` | CMake, option matrix, ODR, public API surface, consumer integration | references/build-api.md |

Every review reads, in this order:
1. `references/context.md`: project facts, how trading uses quark, the hot-path map, the error policy.
2. The area checklist.
3. `references/lessons-learned.md`: real defect patterns found before. Check these first.

## 2. Priorities (in a conflict, the higher one wins)

1. Correctness: no wrong price/quantity/time/state, no UB, no data race, no silent overflow.
2. Predictable stability: judge worst case and tail latency (p99/p999/jitter); failures must be
   deterministic and recoverable.
3. Low latency / high throughput.
4. Maintainability and testability.

Why: in a trading system one occasional wrong result costs far more than being tens of nanoseconds
slower on average, and a good mean with a jittery tail fails exactly during market bursts.

## 3. Hot path definition

Hot = market-data receive and parse, order/cancel send (including signing), risk checks and
fixed-point math, timer firing, one event-loop iteration, inter-thread / inter-process channels,
journal append.

Forbidden on the hot path: malloc/new (including container growth, rehash, `std::string` growth),
system calls (except vDSO `clock_gettime`; a non-blocking `send`/`recv` on the socket being served is
the I/O itself and is expected — count them, don't flag them), locks, exceptions thrown on the normal
path, `std::function` / virtual calls where avoidable, iostream, `shared_ptr` atomic refcounts,
unbounded loops, page faults on memory that was not pre-faulted.

Label each reviewed unit hot / warm / cold with a reason. The same issue has a different severity
in hot vs cold code (`new` at start-up is usually Minor, on the hot path it is Critical/Major). The
map in `context.md` is a starting assumption. Verify it, and report any mismatch.

**Consumer weighting.** quark exists to serve trading. For every finding, say which trading
component is affected (see the consumer map in `context.md`; confirm with
`grep -rn "<Symbol>" /home/ubuntu/personal/trading --include=*.h --include=*.cpp -l | grep -v /build`).
A defect in code that trading runs per message is more urgent than the same defect in unused code;
unused code still gets reviewed, but its severity says "latent (no current consumer)". Code the owner
plans to adopt (ScaleInt for Price/Qty) is "latent-planned": rate it by the impact it will have once
adopted, and say so.

**Target.** The reference production target is the dev box (AMD EPYC 7R13 / Zen 3, see context.md), and
trading builds with `-march=native` + LTO. Latency claims must say which build they come from.

## 4. How to work

1. Read `CMakeLists.txt` and `cmake/` first and treat the actual options as the source of truth. Then
   read the headers in scope, their `tests/` and `benchmarks/`, and the trading call sites. Only
   then draw conclusions.
2. Verify with tools when you can. If you can't, say "static reading only, not verified". When a tool
   is missing (see context.md, "Toolchain on the dev box"), report it as not run and say how to install it. Don't
   pretend the result is known.
   - `${CLAUDE_SKILL_DIR}/scripts/build_matrix.sh [rel asan tsan noexc nologger openssl shared warn]`:
     multi-config build + tests + warning/sanitizer summary (logs in `build/review/logs/`). The first
     run fetches and builds AWS-LC per config; it is slow (minutes per config). Run it in the
     background and report progress.
   - `${CLAUDE_SKILL_DIR}/scripts/coverage.sh`: line/branch coverage per directory (needs gcovr or lcov).
   - `${CLAUDE_SKILL_DIR}/scripts/hotpath_symbols.sh <binary or .o> [function regex]`: scans the
     disassembly for new/malloc/locks/exceptions/128-bit division/syscalls/static-init guards. Code
     that is header-only is instantiated in the target that uses it, so scan the benchmark/test
     binaries (`build/review/rel/benchmarks/...`) and filter with a regex like `quark::` or the
     benchmark name (`BM_`), since inlined code is attributed to the caller.
   - `strace -c -f <test or benchmark>`: syscalls per message.
   - `perf stat -e cycles,instructions,cache-misses,page-faults <benchmark>` for tail-latency claims.
   - clang-tidy (bugprone-*, performance-*, concurrency-*, cert-*) and cppcheck if installed; they need
     `compile_commands.json`, which the `rel` config exports.
   - To assert "zero allocation on the hot path", propose a test that uses `templates/AllocGuard.h`
     (put it in your recommendation; don't add it to the repo yourself).
3. Every finding needs: `file:line`, a concrete trigger (input or interleaving), evidence (standard
   clause / tool output / code reference), confidence (confirmed / highly suspicious / speculative),
   and the affected trading consumer. Speculative findings can't be Critical. If unsure, say so.
   Never invent standard clauses or line numbers.
4. Don't pad the list. For areas with no issues write "no issues found" and what was checked; list
   "designs confirmed good" separately.
5. Suggest newer language features only for a measurable gain or clearly better safety.
   `[[likely]]`-style micro-optimisations need benchmark or perf data. Don't suggest modules.
   Review coroutines/jthread/semaphore only if the code uses them.
6. Vendored code in `include/quark/third_party/` (SPSC_Queue, inplace_function, tscns): review how it
   is integrated and any local modifications, not upstream line by line.
7. Exception policy: start from `QuarkError.h` and "Exception policy target" in context.md. Trading
   builds with exceptions on today, but the target production mode is `QUARK_NO_EXCEPTIONS=ON`, where
   every `QUARK_THROW` aborts. So a throw reachable from **runtime external input** (peer SHM files,
   network data, exchange responses) is at least Major; throws at configuration or start-up are fine.
   Changes in what quark throws are API changes for trading. Plain `assert()` disappears under
   `NDEBUG` (all Release builds, trading included); checks that must survive in production need
   `QUARK_ASSERT` or `QUARK_REQUIRE`.

## 5. Severity

- **Critical**: can produce wrong results, UB, a data race, a crash, or money/order risk.
- **Major**: hot-path latency violation, or a triggerable tail-latency / resource-exhaustion risk.
- **Minor**: local performance or readability issue.
- **Suggestion**: optional improvement.

## 6. Output

Use the structure in `templates/report.md`. Number findings `<AREA>-<NN>` (e.g. `NET-03`) so
`/quark-fix` can refer to them. Print the report in the conversation. Don't write it into the repo
unless the user asks; if they do, use `doc/reviews/<YYYY-MM-DD>-<scope>.md`.

At the end, list "candidate entries for lessons-learned.md", but only if this review found a new,
reusable defect pattern. The user confirms them before they are written.

## 7. triage mode

Read-only triage. No deep review and no fix proposals (except build problems that block later
reviews):

1. Go through `include/`, `src/`, `tests/`, `benchmarks/`, `examples/` and produce a module table:
   module | files | lines | header-only? | depends on | hot/warm/cold (why) | thread model | used by
   trading (which component, per message or setup) | existing tests and benchmarks.
2. Risk score (1–5) = cost of a defect × change frequency (`git log --format= --name-only | sort | uniq -c`)
   × test weakness × trading exposure. Give a Top-10 deep-review order with the argument to use for
   each (math/time/net/containers/parsing/runtime/build).
3. Run `build_matrix.sh rel asan tsan warn` and `coverage.sh`. Summarise the numbers only; don't judge them.
4. List orphan files (not referenced by CMake/tests/examples), local modifications of vendored code
   compared to upstream (fetch upstream into a fresh `mktemp -d` directory and only `diff` it, never build
   or run it), and headers that are not self-contained
   (`${CLAUDE_SKILL_DIR}/scripts/header_selfcontained.sh`, after a `rel` build has fetched the dependencies).
5. List at most 5 questions for the user (e.g. what counts as hot, target CPU and `-march`, whether
   the context.md map is right). Once they are answered, suggest updating `references/context.md`.

## 8. diff mode

1. `git diff --stat <ref>...HEAD` plus `git status --short` (include uncommitted work). Group the
   changed files by area using the scopes in section 1.
2. For each area touched, read its checklist and review **the changed lines and their callers**, not the
   whole area. Also check that the tests and benchmarks changed with the code.
3. For any public header that changed, check the trading call sites (consumer map in context.md) for
   source or behaviour breaks (signature, thrown exceptions, rounding, layout of SHM/journal structs).
4. Verification: at least `build_matrix.sh rel asan`; add `tsan` for concurrency changes, and `noexc
   nologger openssl shared` for changes to headers, macros or CMake options. If a sibling trading
   checkout exists, suggest (don't run unless asked) a trading build against this quark:
   `cmake -S /home/ubuntu/personal/trading -B /tmp/trading-check -DQUARK_SOURCE_DIR=$PWD && cmake --build /tmp/trading-check -j && ctest --test-dir /tmp/trading-check`.
