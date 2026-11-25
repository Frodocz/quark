# math checklist (ScaleInt / Int128 / DoubleOps / MathOps)

Scope: `include/quark/math/*` and where utils/net use them.
Consumer status: **trading doesn't use math today** (Price/Qty are `double` in
`tradecommon/include/tc/Types.h`), but **will move Price/Qty to ScaleInt** (owner 2026-11-25). Report
findings as "latent-planned" and rate them by the impact they will have once ScaleInt carries prices
(a wrong rounding then means a wrong order price). Also check what the move needs: double ↔ ScaleInt
at the JSON boundary (parse straight into fixed point?), the cost on the hot path compared with `double`, and
trading code that would benefit today (`double` prices compared for equality or accumulated).

## Checks
1. Overflow semantics: what does each operation do on overflow (add/sub/mul/div, negate, abs,
   shifts, comparison, scale conversion): detect, saturate, wrap, or signed-overflow UB? Is it
   documented and the same across the library? Key cases: negating MIN, abs(MIN), MIN / -1.
   `MathOps.h` guards abs(MIN) with `assert`, which disappears under NDEBUG: is there a release-mode guard?
2. Intermediate width in multiplication: is the value widened to 128 bits before multiplying by the
   scale? When ScaleInts with different scales are mixed, can the result type or precision be lost silently?
3. Division and rounding: is the rounding mode (toward zero / floor / half-up / banker's) uniform,
   correct for negatives, and documented? 128-bit division calls `__divti3`/`__udivti3`, which is
   slow and has variable latency. Does it appear on the hot path, and could a multiply or shift replace it?
4. double conversions: double→integer out of range, NaN or Inf is UB (`try_from_double` must reject
   it). Do string↔fixed-point round trips lose nothing? Is there any path where a price goes through
   `double` before being compared or accumulated?
5. Constraints and interface: do concepts/requires block dangerous implicit conversions (mixing
   integer and float, implicit conversion between scales)? Are comparison, hash and `<=>`
   consistent? With `CMAKE_CXX_EXTENSIONS=OFF` and `-Wpedantic`, is `__int128` wrapped in
   `__extension__`? Do constexpr/consteval validate constants at compile time?
6. DoubleOps: comparison tolerance, rounding, ULP. MathOps: pow10 table range, integer log/sqrt
   bounds, any UB reachable in constexpr evaluation (it would be a compile error, which is good, but
   check the runtime path too).

## Verification
- Differential tests: random values plus edge values (0, ±1, MIN, MAX, MIN+1, MAX-1, scale limits)
  against `__int128` or Python-generated reference vectors. Overflow paths need "must be detected" tests.
- Compile-fail tests: illegal type mixes must not compile.
- Benchmark: in cycles, against raw int64 and double; report p50/p99/p999, with hot and cold cache.
- `${CLAUDE_SKILL_DIR}/scripts/hotpath_symbols.sh build/review/rel/benchmarks/<math bench> 'quark::|BM_'`
  to look for `__divti3` and similar calls.
