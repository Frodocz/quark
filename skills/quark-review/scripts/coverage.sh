#!/usr/bin/env bash
# Coverage: build with QUARK_CODE_COVERAGE=ON (-O0) in build/review/cov, run the tests, then summarise
# line/branch coverage per file for include/quark and src (third_party excluded).
# Uses gcovr if present, otherwise lcov; if neither is installed it says how to install one.
# Test failures don't stop the report (coverage of a failing suite is still useful); they're reported.
# Environment: QUARK_ROOT, JOBS, CMAKE_ARGS_EXTRA, CTEST_TIMEOUT (same as build_matrix.sh)
set -euo pipefail

ROOT="${QUARK_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
B="$ROOT/build/review/cov"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
CTEST_TIMEOUT="${CTEST_TIMEOUT:-300}"

if ! command -v gcovr >/dev/null 2>&1 && ! command -v lcov >/dev/null 2>&1; then
    echo "error: neither gcovr nor lcov found. Install one: pip install gcovr   (or: sudo apt install lcov)" >&2
    exit 2
fi
mkdir -p "$B"

GEN=()
if [ ! -f "$B/CMakeCache.txt" ] && command -v ninja >/dev/null 2>&1; then GEN=(-G Ninja); fi
# shellcheck disable=SC2206
EXTRA=(${CMAKE_ARGS_EXTRA:-})
cmake -S "$ROOT" -B "$B" "${GEN[@]}" -DCMAKE_BUILD_TYPE=Debug -DQUARK_CODE_COVERAGE=ON "${EXTRA[@]}"
cmake --build "$B" -j"$JOBS"
find "$B" -name '*.gcda' -delete
tests_ok=1
(cd "$B" && ctest --output-on-failure --timeout "$CTEST_TIMEOUT") || tests_ok=0

if command -v gcovr >/dev/null 2>&1; then
    COMMON=(-r "$ROOT" --object-directory "$B"
            --filter "$ROOT/include/quark/" --filter "$ROOT/src/"
            --exclude "$ROOT/include/quark/third_party/"
            --exclude-unreachable-branches --exclude-throw-branches)
    echo "==== line coverage ===="
    gcovr "${COMMON[@]}" --txt | tee "$B/coverage.lines.txt"
    echo "==== branch coverage ===="
    # newer gcovr: --txt-metric branch; older: --branches
    if ! gcovr "${COMMON[@]}" --txt-metric branch >"$B/coverage.branches.txt" 2>/dev/null; then
        gcovr "${COMMON[@]}" --branches >"$B/coverage.branches.txt"
    fi
    cat "$B/coverage.branches.txt"
    echo "reports: $B/coverage.lines.txt  $B/coverage.branches.txt"
else
    lcov --capture --directory "$B" --output-file "$B/coverage.info" --rc lcov_branch_coverage=1
    lcov --extract "$B/coverage.info" "$ROOT/include/quark/*" "$ROOT/src/*" \
         --output-file "$B/coverage.quark.info" --rc lcov_branch_coverage=1
    lcov --remove "$B/coverage.quark.info" "$ROOT/include/quark/third_party/*" \
         --output-file "$B/coverage.filtered.info" --rc lcov_branch_coverage=1
    lcov --list "$B/coverage.filtered.info" --rc lcov_branch_coverage=1 | tee "$B/coverage.lines.txt"
    lcov --summary "$B/coverage.filtered.info" --rc lcov_branch_coverage=1
    echo "report: $B/coverage.lines.txt (HTML: genhtml $B/coverage.filtered.info -o $B/html)"
fi

if [ "$tests_ok" -eq 0 ]; then
    echo "WARNING: some tests failed; coverage above reflects a failing run." >&2
    exit 1
fi
