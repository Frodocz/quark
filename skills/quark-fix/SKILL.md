---
name: quark-fix
description: Fix phase for the quark crypto HFT base library. Applies only the user-confirmed findings from a quark-review report, test-first, one finding per commit, with before/after benchmarks for performance changes and a check that the trading modules still build. Use only after the user has confirmed which review findings to fix.
argument-hint: "<finding IDs, e.g. NET-02 TIME-01> [report path, e.g. doc/reviews/2026-11-24-net.md]"
disable-model-invocation: true
---

# quark-fix (fix phase)

Use this only after the user has confirmed a findings list and said which IDs to fix. For
reviews use `/quark-review`.

Arguments (finding IDs to fix, and optionally where the report is): $ARGUMENTS

Before starting, read:
1. `${CLAUDE_SKILL_DIR}/../quark-review/references/context.md`: project facts, the consumer map,
   the error policy.
2. The confirmed findings: the report file given in the arguments, or the quark-review report
   earlier in this conversation. If you can't find a confirmed list or the IDs don't match any
   finding, stop and ask the user. Never pick findings to fix yourself.
3. `${CLAUDE_SKILL_DIR}/../quark-review/references/lessons-learned.md` for patterns related to the fix.

Scripts are in `${CLAUDE_SKILL_DIR}/../quark-review/scripts/`. Run them from the quark repo root.

## Rules

1. **Scope.** Fix only the IDs the user picked and leave everything else alone. Record new issues you
   notice under "new findings" and don't fix them on the side. Why: changes that grow beyond their
   scope can't be reviewed or rolled back safely in a trading system.
2. **Branch.** If on `main`, create `fix/<area>-<ids>` first (e.g. `fix/net-02-03`). Never push or
   open a PR unless the user asks.
3. **Test first.** One finding (or one tightly coupled group) at a time: write a test that fails and
   reproduces it, change the code, confirm the test passes. If no failing test is possible (pure
   performance work), write a benchmark first and record the baseline. Put tests next to the
   existing ones (`tests/<area>/<Module>Test.cpp`, registered in `tests/CMakeLists.txt` with
   `quark_add_test`) and benchmarks under `benchmarks/<area>/`.
4. **Behaviour contracts need consent.** Without the user's approval, don't change any of:
   - the public API (names, signatures, what throws vs returns an error: trading catches
     `simdjson_error`/`std::exception` around quark calls);
   - serialisation and shared-memory layouts (`FullDuplexShm`/`SPSCVarQueue` must stay
     byte-compatible with the legacy binaries; the persisted `Journal` record format);
   - rounding or overflow semantics;
   - CMake option names and defaults, or PUBLIC compile definitions.
   If a fix needs one of these, stop, explain the impact (including which trading files change)
   and wait.
5. **Performance changes need before/after numbers.** Cycles, p50/p99/p999, same machine, same
   pinning and warm-up (see `doc/TUNING.md`). If you can't show it's better, don't make the
   change, and say so.
6. **Verify after each fix:**
   - `build_matrix.sh rel asan noexc`; add `tsan` for concurrency changes, and `nologger openssl shared`
     for changes to headers, macros or options. These are slow (AWS-LC builds per config), so run
     them in the background.
   - Compare with the state before the fix: test results, warning count, sanitizer reports, and
     coverage for the files you touched.
   - For hot-path changes, run `hotpath_symbols.sh` on the affected benchmark/test binary again and
     confirm there are no new allocation/lock/exception/128-bit-division/static-guard call sites.
   - **Consumer check.** If a public header changed, build and test trading against the working tree:
     `cmake -S /home/ubuntu/personal/trading -B build/review/trading -DQUARK_SOURCE_DIR=$PWD && cmake --build build/review/trading -j && ctest --test-dir build/review/trading --output-on-failure`.
     Trading builds with `-Werror`, so new warnings in quark headers break it. Never edit trading
     from this skill; report the breaks it needs.
7. **One finding per commit.** The message covers: finding ID, root cause, the test that guards
   it, risk, and how to roll it back. Example:
   ```
   fix(net): NET-02 WsSession keeps a dangling view after reconnect

   Root cause: ...
   Test: tests/net/WsSessionTest.cpp ReconnectDoesNotReuseView
   Risk: low, the receive path only; no API/layout change
   Rollback: revert this commit
   ```
8. Keep the project's style and error-handling conventions (`QUARK_THROW`/`try_*` APIs, `QUARK_ASSERT`
   for checks that must stay in Release). Add no new dependencies, and don't rewrite code just to use
   newer language features. Update `CHANGELOG.md` if the change is visible to users.

## Final output

- Fixed: ID, commit, test, verification results (configs run, before → after)
- Not fixed, and why
- New findings (not handled)
- Decisions needed from the user
- Trading impact: whether the consumer build ran and what changed for trading
- If the fix exposed a reusable defect pattern, a proposed entry for
  `quark-review/references/lessons-learned.md` (the user confirms it before it is written)
