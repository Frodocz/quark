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

# The C compiler must match the C++ compiler: AWS-LC is C, and CMake adds the C compiler's library
# dir (-L.../gcc/<ver>) to C++ links. With cc=gcc-12 and c++=g++-11 (as on the dev box), -lgcov/-lgcc then
# come from GCC 12 (coverage silently records nothing) and LTO objects of two GCC versions can't be linked.
# Prints a -DCMAKE_C_COMPILER=... argument for the given C++ compiler, or nothing if CC or
# CMAKE_ARGS_EXTRA already chooses one.
c_compiler_arg() {
    local cxx="$1" c ver
    [ -n "${CC:-}" ] && { echo "-DCMAKE_C_COMPILER=$CC"; return; }
    [[ "${CMAKE_ARGS_EXTRA:-}" == *CMAKE_C_COMPILER* ]] && return
    cxx=$(command -v "$cxx" 2>/dev/null) || return
    if "$cxx" --version 2>/dev/null | grep -qi clang; then
        c="${cxx%++}"                      # clang++ -> clang, clang++-14 -> clang-14 handled below
        [ -x "$c" ] || c="${cxx/clang++/clang}"
    else
        ver=$("$cxx" -dumpversion 2>/dev/null | cut -d. -f1)
        c=$(command -v "gcc-$ver" 2>/dev/null || true)
    fi
    [ -n "$c" ] && [ -x "$c" ] && echo "-DCMAKE_C_COMPILER=$c"
}

CARG=$(c_compiler_arg "${CXX:-c++}")
# A dir first configured with another C compiler: start clean, CMake can't switch compilers in place
if [ -f "$B/CMakeCache.txt" ] && [ -n "$CARG" ] && ! grep -q "^CMAKE_C_COMPILER:FILEPATH=${CARG#-DCMAKE_C_COMPILER=}$" "$B/CMakeCache.txt"; then
    echo "note: $B was configured with another C compiler, reconfiguring from scratch"
    rm -rf "$B"; mkdir -p "$B"
fi

# shellcheck disable=SC2206
EXTRA=(${CMAKE_ARGS_EXTRA:-})
GEN=()
if [ ! -f "$B/CMakeCache.txt" ] && command -v ninja >/dev/null 2>&1; then GEN=(-G Ninja); fi
CARGS=()
[ -n "$CARG" ] && CARGS+=("$CARG")
cmake -S "$ROOT" -B "$B" "${GEN[@]}" -DCMAKE_BUILD_TYPE=Debug -DQUARK_CODE_COVERAGE=ON "${CARGS[@]}" "${EXTRA[@]}"
cmake --build "$B" -j"$JOBS"
find "$B" -name '*.gcda' -delete
tests_ok=1
(cd "$B" && ctest --output-on-failure --timeout "$CTEST_TIMEOUT") || tests_ok=0

if [ -z "$(find "$B" -name '*.gcda' -print -quit)" ]; then
    echo "error: the tests wrote no .gcda files (check the test log for 'libgcov profiling error', e.g. mixed GCC versions)" >&2
    exit 3
fi

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
