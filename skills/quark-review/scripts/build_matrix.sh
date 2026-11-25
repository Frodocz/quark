#!/usr/bin/env bash
# Multi-config build + test + summary. Each config gets its own directory build/review/<name>;
# logs go to build/review/logs/.
#
# Usage: build_matrix.sh [config...]
#   configs: rel asan tsan noexc nologger openssl shared warn
#   with no arguments: rel asan tsan warn
#
#   rel       Release + benchmarks + compile_commands.json (for clang-tidy / hotpath_symbols.sh)
#   asan      ASan+UBSan, -O2 -g WITHOUT -DNDEBUG, so header assert()s are live
#   tsan      TSan, -O2 -g without NDEBUG; clang++ if available (GCC TSan may report false positives on fences)
#   noexc     QUARK_NO_EXCEPTIONS=ON
#   nologger  QUARK_ENABLE_LOGGER=OFF
#   openssl   QUARK_TLS_BACKEND=OpenSSL
#   shared    BUILD_SHARED_LIBS=ON
#   warn      extra warnings ($WARN_FLAGS). Test/bench targets build with -Werror, so new warnings
#             there become errors: the build keeps going and the summary counts them separately.
#             quark's own objects are deleted first so a re-run reports every diagnostic again.
#
# Environment:
#   QUARK_ROOT        repo root (default: git root or the current directory)
#   JOBS              parallel compile jobs (default: nproc)
#   WARN_FLAGS        extra warning flags for the warn config
#   CXX               C++ compiler for all configs except tsan (default: CMake's choice, i.e. c++)
#   CC                C compiler; by default the one matching the C++ compiler (g++-11 -> gcc-11,
#                     clang++ -> clang), because mixed GCC versions break coverage and LTO
#   CXX_TSAN          compiler for tsan (default clang++, falls back to the default compiler with a note)
#   CMAKE_ARGS_EXTRA  extra cmake arguments for every config (word-split)
#   CTEST_TIMEOUT     per-test timeout in seconds (default 300)
#
# The first configure fetches dependencies over the network (FetchContent). The sources fetched by
# the first successful config of a run are reused by the later configs in the same run (same tags), so
# only one config downloads. AWS-LC is still compiled once per config, which takes a few minutes. Offline:
# pass -DFETCHCONTENT_SOURCE_DIR_<NAME>=<path> through CMAKE_ARGS_EXTRA.
set -uo pipefail

ROOT="${QUARK_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
OUT="$ROOT/build/review"
LOG="$OUT/logs"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
WARN_FLAGS="${WARN_FLAGS:--Wconversion -Wshadow -Wnull-dereference -Wold-style-cast -Wcast-align -Wdouble-promotion}"
CXX_TSAN="${CXX_TSAN:-clang++}"
CTEST_TIMEOUT="${CTEST_TIMEOUT:-300}"
mkdir -p "$LOG"

for tool in cmake ctest; do
    command -v "$tool" >/dev/null 2>&1 || { echo "error: $tool not found, install it first (e.g. apt install cmake)." >&2; exit 127; }
done

GEN=()
KEEP_GOING=(-k)
if command -v ninja >/dev/null 2>&1 && [[ "${CMAKE_ARGS_EXTRA:-}" != *"-G"* ]]; then
    GEN=(-G Ninja)
    KEEP_GOING=(-k 0)
fi

# Sanitizer runtimes of older GCC fail on kernels with high mmap ASLR entropy; disable ASLR for the test run.
SAN_RUN=()
if command -v setarch >/dev/null 2>&1; then SAN_RUN=(setarch "$(uname -m)" -R); fi

# Only count diagnostics in quark's own code, not in fetched dependencies.
PROJECT_DIAG_RE="^($ROOT/)?(include|src|tests|benchmarks|examples)/"

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

if [ "$#" -eq 0 ]; then set -- rel asan tsan warn; fi

declare -a SUMMARY_NAMES=() SUMMARY_ROWS=() FETCH_REUSE=()
overall=0

collect_fetched_sources() {
    # FetchContent_MakeAvailable(<name>) honours FETCHCONTENT_SOURCE_DIR_<NAME-uppercased>
    local bdir="$1" d name
    [ "${#FETCH_REUSE[@]}" -gt 0 ] && return
    for d in "$bdir"/_deps/*-src; do
        [ -d "$d" ] || continue
        name=$(basename "$d"); name="${name%-src}"
        FETCH_REUSE+=("-DFETCHCONTENT_SOURCE_DIR_${name^^}=$d")
    done
}

run_config() {
    local name="$1"
    local -a args=()
    local note="" san_cfg=0 werror_cfg=0 cxx_used="${CXX:-c++}"
    local sanflags="-O2 -g"   # RelWithDebInfo without -DNDEBUG: assert() stays active
    case "$name" in
        rel)      args=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DQUARK_BUILD_BENCHMARKS=ON) ;;
        asan)     args=(-DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=$sanflags" -DQUARK_SANITIZE_ADDRESS=ON)
                  san_cfg=1 ;;
        tsan)     args=(-DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=$sanflags" -DQUARK_SANITIZE_THREAD=ON)
                  san_cfg=1
                  if command -v "$CXX_TSAN" >/dev/null 2>&1; then
                      args+=("-DCMAKE_CXX_COMPILER=$(command -v "$CXX_TSAN")")
                      cxx_used="$CXX_TSAN"
                  else
                      note="$CXX_TSAN not found, using the default compiler (GCC TSan may report false positives on atomic_thread_fence)"
                  fi ;;
        noexc)    args=(-DCMAKE_BUILD_TYPE=Release -DQUARK_NO_EXCEPTIONS=ON) ;;
        nologger) args=(-DCMAKE_BUILD_TYPE=Release -DQUARK_ENABLE_LOGGER=OFF) ;;
        openssl)  args=(-DCMAKE_BUILD_TYPE=Release -DQUARK_TLS_BACKEND=OpenSSL) ;;
        shared)   args=(-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON) ;;
        warn)     args=(-DCMAKE_BUILD_TYPE=Release -DQUARK_BUILD_BENCHMARKS=ON "-DCMAKE_CXX_FLAGS=$WARN_FLAGS")
                  werror_cfg=1 ;;
        *) echo "unknown config: $name (choose from: rel asan tsan noexc nologger openssl shared warn)" >&2
           SUMMARY_NAMES+=("$name"); SUMMARY_ROWS+=("unknown|-|-|-|-"); overall=1; return ;;
    esac

    local carg; carg=$(c_compiler_arg "$cxx_used")
    [ -n "$carg" ] && args+=("$carg")
    [ -n "${CXX:-}" ] && [ "$cxx_used" = "$CXX" ] && args+=("-DCMAKE_CXX_COMPILER=$(command -v "$CXX")")

    # shellcheck disable=SC2206
    local -a extra=(${CMAKE_ARGS_EXTRA:-})
    local bdir="$OUT/$name"
    local cfg_log="$LOG/$name.configure.log" build_log="$LOG/$name.build.log" test_log="$LOG/$name.test.log"
    local status_build="ok" status_test="ok"
    rm -f "$test_log"

    local -a gen=()
    [ -f "$bdir/CMakeCache.txt" ] || gen=("${GEN[@]}")   # never switch the generator of an existing dir

    echo "==== [$name] configure ${note:+($note)}"
    if ! cmake -S "$ROOT" -B "$bdir" "${gen[@]}" "${args[@]}" "${FETCH_REUSE[@]}" "${extra[@]}" >"$cfg_log" 2>&1; then
        echo "     configure failed, see $cfg_log (network problem? see the script header)"
        SUMMARY_NAMES+=("$name"); SUMMARY_ROWS+=("configure FAIL|-|-|-|$bdir"); overall=1; return
    fi
    collect_fetched_sources "$bdir"

    echo "==== [$name] build"
    local -a kg=()
    if [ "$werror_cfg" -eq 1 ]; then
        kg=(-- "${KEEP_GOING[@]}")
        # An incremental build re-prints no warnings for objects that are up to date, so counts would
        # drop to 0 on a re-run. Delete quark's own objects (not those of _deps/, e.g. AWS-LC) so every
        # quark TU is compiled, and its diagnostics printed, again.
        find "$bdir" -path "$bdir/_deps" -prune -o -name '*.o' -type f -print0 | xargs -0 -r rm -f
    fi
    if ! cmake --build "$bdir" -j"$JOBS" "${kg[@]}" >"$build_log" 2>&1; then
        status_build="FAIL"; overall=1
    fi

    local warns werrs
    warns=$(grep -E "warning:" "$build_log" 2>/dev/null | grep -Ec "$PROJECT_DIAG_RE" || true)
    werrs=$(grep -E "error:.*\[-Werror" "$build_log" 2>/dev/null | grep -Ec "$PROJECT_DIAG_RE" || true)
    if [ "$werror_cfg" -eq 1 ] && [ "$status_build" = "FAIL" ] && [ "${werrs:-0}" -gt 0 ]; then
        status_build="Werror"   # expected under strict flags: the count is the result
    fi

    if [ "$status_build" = "ok" ]; then
        echo "==== [$name] ctest"
        local -a runner=()
        [ "$san_cfg" -eq 1 ] && runner=("${SAN_RUN[@]}")
        if ! (cd "$bdir" && ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}" \
                UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1}" \
                TSAN_OPTIONS="${TSAN_OPTIONS:-second_deadlock_stack=1}" \
                "${runner[@]}" ctest --output-on-failure --timeout "$CTEST_TIMEOUT" >"$test_log" 2>&1); then
            status_test="FAIL"; overall=1
        fi
    else
        status_test="skipped"
    fi

    local san=0
    if [ -f "$test_log" ]; then
        san=$(grep -Ec "ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:|WARNING: ThreadSanitizer" "$test_log" 2>/dev/null || true)
    fi

    SUMMARY_NAMES+=("$name")
    SUMMARY_ROWS+=("build:$status_build test:$status_test|$warns|$werrs|$san|$bdir")

    if [ "${warns:-0}" -gt 0 ] || [ "${werrs:-0}" -gt 0 ]; then
        echo "     top 10 diagnostic categories (quark code only):"
        grep -E "warning:|error:.*\[-Werror" "$build_log" | grep -E "$PROJECT_DIAG_RE" \
            | grep -o "\[-W[a-zA-Z0-9=+-]*\]" | sort | uniq -c | sort -rn | head -10 | sed 's/^/       /'
        echo "     top 10 files:"
        grep -E "warning:|error:.*\[-Werror" "$build_log" | grep -E "$PROJECT_DIAG_RE" \
            | cut -d: -f1 | sed "s|^$ROOT/||" | sort | uniq -c | sort -rn | head -10 | sed 's/^/       /'
    fi
    if [ "$status_test" = "FAIL" ]; then
        echo "     failed tests:"
        grep -E "^\s*[0-9]+ - .*\((Failed|Timeout|SEGFAULT|Subprocess aborted|Exception)" "$test_log" | head -20 | sed 's/^/       /'
    fi
    [ -n "$note" ] && echo "     note: $note"
}

for c in "$@"; do run_config "$c"; done

echo
echo "================ summary ================"
printf '%-10s %-28s %-9s %-9s %-10s %s\n' "config" "status" "warnings" "Werror" "sanitizer" "build dir"
for i in "${!SUMMARY_NAMES[@]}"; do
    IFS='|' read -r st w e s d <<<"${SUMMARY_ROWS[$i]}"
    printf '%-10s %-28s %-9s %-9s %-10s %s\n' "${SUMMARY_NAMES[$i]}" "$st" "$w" "$e" "$s" "$d"
done
echo "(warnings/Werror count diagnostics in quark's include/ src/ tests/ benchmarks/ examples/ only)"
echo "logs: $LOG"
exit "$overall"
