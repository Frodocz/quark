# quark-review report: <scope/argument> (<YYYY-MM-DD>, quark <git short sha>)

## 1. Summary
- Verdict: ready to merge / merge after small fixes / needs rework
- Key points (≤ 5):
  1.
- Designs confirmed good:

## 2. Verification
| Item | Run? | Result |
|------|------|--------|
| rel build + tests | | |
| asan (ASan+UBSan) | | |
| tsan (compiler: clang/GCC) | | |
| strict warnings (warn) | | |
| coverage (line/branch, per directory) | | |
| hot-path symbol scan | | |
| clang-tidy / cppcheck | | |
| trading build against this quark | | |
For items not run, give the reason (e.g. tool missing) and state "static reading only, not verified".

## 3. Hot-path classification check
Where the code disagrees with the starting map in context.md:

## 4. Findings (Critical → Major → Minor → Suggestion)
### [<AREA>-<NN>] <title>
- Severity / impact (correctness | latency | stability | maintainability):
- Location (file:line / symbol):
- Affected trading consumer (component + per-message/setup, or "latent: no current consumer"):
- Description and trigger (input or interleaving):
- Evidence (standard clause / engineering reasoning / tool output):
- Confidence (confirmed | highly suspicious | speculative):
- Suggested fix (and whether it changes public API, SHM/journal layout, rounding/overflow or exception behaviour):

## 5. Refactoring examples (Critical/Major only)
Compilable C++20 snippets, with the behaviour difference and the performance impact.

## 6. Tests and benchmarks to add (by value)
| Priority | Case | Defect class it guards |
|----------|------|------------------------|

## 7. Fix order and dependencies
Which IDs to fix together, and in what order. This is the list `/quark-fix` takes.

## 8. Candidate lessons-learned.md entries (optional)
