# HOW TO: review quark with Claude Code using these skills

This folder holds two Claude Code skills, used as a standard operating procedure (SOP) for reviewing and fixing quark:

| Skill | What it does | Changes files? |
|---|---|---|
| `/quark-review <scope>` | A read-only review of one area of quark, weighted by how the trading modules (alphaframe, tradeserver, backtest, tradecommon) use it | No |
| `/quark-fix <IDs> [report]` | Fixes the findings you confirmed: test first, one commit per finding, then re-verified, including a build of trading against the change | Yes (on a `fix/...` branch) |

```
skills/
├── HOW_TO.md                      ← this file
├── quark-review/
│   ├── SKILL.md                   ← routing, priorities, hot-path rules, severity, triage/diff modes
│   ├── references/
│   │   ├── context.md             ← project facts + how trading uses quark + hot-path map (KEEP UPDATED)
│   │   ├── lessons-learned.md     ← real defect patterns, checked first in every review (KEEP UPDATED)
│   │   ├── math.md  time.md  net.md  containers.md  parsing-crypto.md  runtime.md  build-api.md
│   ├── scripts/
│   │   ├── build_matrix.sh        ← multi-config build + test + warning/sanitizer summary
│   │   ├── coverage.sh            ← line/branch coverage (gcovr or lcov)
│   │   └── hotpath_symbols.sh     ← finds new/malloc/locks/throw/__divti3/... call sites in binaries
│   └── templates/
│       ├── report.md              ← the report layout (finding IDs like NET-03)
│       └── AllocGuard.h           ← test helper that asserts "zero allocations"
└── quark-fix/
    └── SKILL.md
```

---

## 1. One-time setup

### 1.1 Make Claude Code find the skills

Claude Code only loads skills from `.claude/skills/` (per project) or `~/.claude/skills/` (per user).
A top-level `skills/` folder is **not** picked up by itself. Link it into the project:

```bash
cd /home/ubuntu/personal/quark
mkdir -p .claude
ln -s ../skills .claude/skills        # .claude/skills/quark-review/SKILL.md now resolves
```

Restart Claude Code in `/home/ubuntu/personal/quark` and type `/` to check that `quark-review` and
`quark-fix` are listed. (Use a symlink, not a copy, so `skills/` stays the single version-controlled copy.)

Both skills set `disable-model-invocation: true`, so Claude never starts them on its own. Only
typing `/quark-review` or `/quark-fix` runs them. That's on purpose: a review run can take a long
time, and a fix run commits.

### 1.2 Install the missing tools (optional but recommended)

The dev box currently has GCC 11.4, cmake, ninja, objdump, strace and perf. Without the tools
below, the review still works but reports those checks as "not run":

```bash
pip install gcovr                                   # coverage.sh
sudo apt install clang clang-tidy cppcheck lcov     # clang TSan (fewer false positives), static analysis, libFuzzer
sudo apt install valgrind heaptrack ltrace          # optional: allocation tracing
```

With GCC 11.4 only the fmt path of `QuarkFormat` is built; covering `std::format` needs GCC ≥ 13 or Clang ≥ 16.

### 1.3 Allow the scripts without prompts (optional)

To stop Claude Code from asking every time it runs a script, add to `.claude/settings.local.json`:

```json
{ "permissions": { "allow": [
  "Bash(skills/quark-review/scripts/build_matrix.sh:*)",
  "Bash(skills/quark-review/scripts/coverage.sh:*)",
  "Bash(skills/quark-review/scripts/hotpath_symbols.sh:*)",
  "Bash(.claude/skills/quark-review/scripts/build_matrix.sh:*)",
  "Bash(.claude/skills/quark-review/scripts/coverage.sh:*)",
  "Bash(.claude/skills/quark-review/scripts/hotpath_symbols.sh:*)"
] } }
```

---

## 2. The SOP: the review cycle

```
 ┌─ (1) /quark-review triage ──► answer ≤5 questions ──► update references/context.md
 │
 ├─ (2) for each area in the Top-10 order:
 │        /quark-review <area>  ──► report with IDs ──► you mark which IDs to fix
 │        (save it: doc/reviews/<date>-<area>.md)
 │
 ├─ (3) /quark-fix <IDs> doc/reviews/<date>-<area>.md ──► branch + commits + verification
 │
 ├─ (4) accept or reject the proposed lessons-learned entries ──► references/lessons-learned.md
 │
 └─ (5) ongoing: /quark-review diff before merging any quark change
```

### Step 1: triage (once, and again after large changes)

```
/quark-review triage
```

Claude builds a module table (lines, header-only?, hot/warm/cold, thread model, **which trading
component uses it**, tests and benchmarks), scores risk, and returns a Top-10 deep-review order
with the argument to use for each. It runs `build_matrix.sh rel asan tsan warn` and `coverage.sh`
and reports only the numbers. The build matrix takes a while because AWS-LC is compiled once per
config, so let it run in the background.

It ends with at most 5 questions (target CPU / `-march`, whether trading will move to ScaleInt,
whether `QUARK_NO_EXCEPTIONS` is a production mode...). Answer them, then ask Claude to update
`skills/quark-review/references/context.md` with your answers (for example: *"update context.md with
these answers"*). Every later review reads that file first, so its accuracy decides how good the
reviews are.

### Step 2: area reviews

Run one area per session (or `/clear` between them) so each review has a clean context:

```
/quark-review net
/quark-review containers
/quark-review parsing
/quark-review time
/quark-review runtime
/quark-review build
/quark-review math
```

A suggested order, based on what trading runs per message today (see the consumer map in
`context.md`):

1. `net`: WsSession/WsGroup/TlsStream carry every MD message and every order.
2. `containers`: FullDuplexShm (cross-process ABI), Journal (written on the loop thread, synced on another thread), TimerQueue, pools.
3. `parsing`: JsonParser on every message and HmacSha256 on every Binance order.
4. `time`: RdtscClock stamps everything; RateLimiter gates every order.
5. `runtime`: the log producer side, config, the fatal path.
6. `build`: options, ODR, how trading consumes quark through `add_subdirectory`.
7. `math`: trading doesn't use ScaleInt yet, so findings are "latent" unless you plan to adopt it.

You can steer a review in the same message:

```
/quark-review net
Focus on WsGroup::send_best and reconnect; static reading only, don't run the build matrix.
```

Each report follows `templates/report.md`: a summary, a verification table, a hot-path
classification check, and findings numbered `NET-01`, `NET-02`, ... Each finding gives severity,
`file:line`, the affected trading consumer, the trigger, evidence, confidence and a suggested fix.
Speculative findings can't be rated Critical.

**Save the report** if you'll fix things in a later session:

```
save this report to doc/reviews/2026-11-24-net.md
```

(The review skill never writes files on its own; it only does when you ask.)

### Step 3: fix what you confirm

Read the report, decide, then:

```
/quark-fix NET-02 NET-05 doc/reviews/2026-11-24-net.md
```

Or, in the same session as the review, just `/quark-fix NET-02 NET-05`. What `quark-fix` does:

- creates a `fix/net-02-05` branch when you're on `main`;
- per finding: a failing test (or a baseline benchmark), then the fix, then the test passes;
- **stops and asks** before changing the public API, the SHM/Journal layout, rounding/overflow
  semantics, what throws vs returns an error, or CMake options;
- re-verifies with `build_matrix.sh` (rel asan noexc, plus tsan / nologger openssl shared when relevant)
  and `hotpath_symbols.sh` for hot-path changes;
- builds and tests **trading against the modified quark** (`-DQUARK_SOURCE_DIR=$PWD`); it never edits trading;
- makes one commit per finding (message: ID, root cause, test, risk, rollback), and never pushes.

It ends with a fixed / not-fixed / new-findings / decisions-needed / trading-impact summary.

### Step 4: grow lessons-learned

Both skills may propose "candidate lessons-learned entries". If one is a real, reusable pattern, tell
Claude: *"add LL candidate 1 to lessons-learned.md"*. Every future review checks these patterns
first. LL-001…LL-005 were seeded from git history (f6f28b1) and trading's
`IMPLEMENTATION_SUMMARY.md`: **confirm or delete them**.

### Step 5: ongoing reviews of changes

Before merging any quark change (yours or Claude's):

```
/quark-review diff            # vs main (or HEAD~1 when on main), including uncommitted work
/quark-review diff f2b1787    # vs a specific commit
```

It maps the changed files to areas, reviews only the changed code and its callers against those
areas' checklists, checks trading call sites of changed public headers, and runs at least `rel asan`.

---

## 3. Running the scripts by hand

From the quark repo root:

```bash
S=skills/quark-review/scripts

$S/build_matrix.sh                       # rel asan tsan warn
$S/build_matrix.sh rel noexc openssl     # pick configs: rel asan tsan noexc nologger openssl shared warn
JOBS=4 $S/build_matrix.sh asan           # limit parallelism
# results: build/review/<config>/, logs in build/review/logs/<config>.{configure,build,test}.log

$S/coverage.sh                           # build/review/cov/coverage.{lines,branches}.txt

# after `build_matrix.sh rel` (which builds the benchmarks):
$S/hotpath_symbols.sh build/review/rel/benchmarks/<bench binary> 'BM_|quark::'
```

Notes:
- `asan`/`tsan` build at `-O2 -g` **without `NDEBUG`**, so header `assert()`s are active.
  (`QUARK_ENABLE_ASSERTIONS` doesn't do anything today; see `build-api.md`, item 1.)
- `warn` adds `-Wconversion -Wshadow ...`. Test and benchmark targets build with `-Werror`, so new
  warnings show up there as errors. The summary counts warnings and `-Werror` errors in quark's
  own code only (not in dependencies).
- Within one run, sources fetched by the first config are reused by the others; only one config downloads.
- Sanitizer tests run under `setarch -R` (ASLR off) to avoid GCC 11 sanitizer start-up failures on newer kernels.

---

## 4. Maintaining the skills

| When | Update |
|---|---|
| Trading starts or stops using a quark component (e.g. moves prices to ScaleInt) | the consumer map and hot-path map in `references/context.md`. Refresh it with the `grep` command at the bottom of the consumer map |
| A build option, dependency version or the toolchain changes | "Build and options" / "Toolchain on the dev box" in `context.md` |
| A review finds a real, reusable bug pattern | an entry in `references/lessons-learned.md` |
| A new module is added to quark | add it to the scope line of the matching area checklist and to the hot-path map |
| A checklist item keeps producing false positives | reword or delete it; checklists should stay short and specific |

Tips:
- **Keep context small.** One area per session. The skill loads only the checklist it needs.
- **Effort.** For deep areas (`net`, `containers`) a higher-effort model or setting gives better
  concurrency and memory-order reasoning.
- **Trust, but verify.** Every finding cites `file:line` and a confidence level. Open the line before
  you confirm a fix, especially for "highly suspicious" ones.
- **Reviewing from the trading repo.** The skills live in quark. To run them while working in
  `/home/ubuntu/personal/trading`, either start Claude Code in quark, or link them for your user:
  `ln -s /home/ubuntu/personal/quark/skills/quark-review ~/.claude/skills/quark-review` (and the same
  for `quark-fix`). The scripts use the git root of the current directory, so run them with
  `QUARK_ROOT=/home/ubuntu/personal/quark`.
