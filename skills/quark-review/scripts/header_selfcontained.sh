#!/usr/bin/env bash
# Checks that every public quark header is self-contained: it compiles when it is the only include
# of a TU, and when it is included twice (include guard / #pragma once). One TU per header, syntax only.
#
# Usage: header_selfcontained.sh [build dir with fetched deps] [extra compiler flags...]
#   build dir: where FetchContent put quill/simdjson/magic_enum/aws-lc (default: build/review/rel,
#              i.e. run `build_matrix.sh rel` first). Its _deps/*-src/include dirs are added with -isystem.
#   extra flags, e.g. -DQUARK_NO_EXCEPTIONS -fno-exceptions, or -march=native.
#
# Environment: QUARK_ROOT (default: git root or cwd), CXX (default g++), JOBS (default nproc).
# include/quark/third_party/ is skipped (vendored). FStack.h is checked only with -DQUARK_USE_FSTACK
# in the extra flags, since it needs F-Stack's ff_api.h.
set -uo pipefail

ROOT="${QUARK_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
BDIR="${1:-$ROOT/build/review/rel}"
[ "$#" -gt 0 ] && shift
CXX="${CXX:-g++}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
command -v "$CXX" >/dev/null 2>&1 || { echo "error: compiler $CXX not found" >&2; exit 127; }
[ -d "$BDIR/_deps" ] || { echo "error: $BDIR/_deps not found; run build_matrix.sh rel first, or pass a build dir" >&2; exit 2; }

INC=(-I"$ROOT/include")
for d in "$BDIR"/_deps/*-src/include "$BDIR"/_deps/*-src/singleheader; do
    [ -d "$d" ] && INC+=(-isystem "$d")
done
# The PUBLIC compile definitions quark gives its consumers (CMakeLists.txt), with default values
DEFS=(-DQUARK_LOG_QUEUE_TYPE=BoundedDropping -DQUARK_LOG_INITIAL_QUEUE_CAPACITY=2097152 -DQUARK_LOG_HUGE_PAGES=1)
EXTRA=("$@")

check_fstack=0
for f in "${EXTRA[@]}"; do [ "$f" = "-DQUARK_USE_FSTACK" ] && check_fstack=1; done

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mapfile -t HEADERS < <(cd "$ROOT/include" && find quark -name '*.h' -not -path 'quark/third_party/*' | sort)
for h in "${HEADERS[@]}"; do
    if [ "$(basename "$h")" = "FStack.h" ] && [ "$check_fstack" -eq 0 ]; then continue; fi
    tu="$TMP/$(echo "$h" | tr '/.' '__').cpp"
    printf '#include "%s"\n#include "%s"\n' "$h" "$h" >"$tu"
    printf '%s\0' "$tu"
done >"$TMP/list"

fail=0
# Run the compiles in parallel; each writes <tu>.err on failure
xargs -0 -P "$JOBS" -I{} sh -c '"$0" -std=c++20 -fsyntax-only "$@" "{}" 2>"{}.err" || touch "{}.failed"' \
    "$CXX" "${DEFS[@]}" "${INC[@]}" "${EXTRA[@]}" <"$TMP/list"

for marker in "$TMP"/*.failed; do
    [ -e "$marker" ] || continue
    fail=$((fail + 1))
    tu="${marker%.failed}"
    hdr=$(head -n1 "$tu" | sed -E 's/#include "(.*)"/\1/')
    echo "FAIL include/$hdr"
    grep -m3 -E "error|fatal" "$tu.err" | sed 's/^/    /'
done

checked=$(tr -cd '\0' <"$TMP/list" | wc -c)
echo "checked $checked headers with $CXX ${EXTRA[*]:-}: $fail not self-contained"
[ "$fail" -eq 0 ]
