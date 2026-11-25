#!/usr/bin/env bash
# Scans disassembly for call sites that shouldn't appear on a hot path:
#   operator new/delete, the malloc family, exceptions (__cxa_throw, ...), mutexes/condition
#   variables, 128-bit division (__divti3, ...), std::function calls, iostream/locale,
#   std::string growth, common blocking/syscall wrappers, function-local static guards
#   (__cxa_guard_acquire), and TLS access through __tls_get_addr (shared builds).
#
# Usage: hotpath_symbols.sh <binary or .o> [function regex]
#   The second argument filters by the (demangled) name of the function the call is in, e.g.
#   'quark::' or 'TimerQueue'. Header-only code is often inlined into the caller, so also try the
#   caller's name, e.g. 'BM_' for benchmarks or 'TestBody' for gtest bodies.
#   Without a filter every function is scanned (very noisy; always filter).
#
# Notes:
#   - Header-only code is instantiated in the target that uses it, so scan benchmark / test
#     executables or .o files (e.g. build/review/rel/benchmarks/...).
#   - Hits are leads, not proof: a hit isn't necessarily on the hot path (it may be in a cold
#     branch); no hit doesn't mean there's none (inlining, indirect calls, virtual calls).
#     Read the surrounding code for each hit before reporting it.
#   - send/recv/read/write on the socket being served are the I/O itself; report their count,
#     not their presence.
#   - In .o files the call target is on the relocation line; in linked binaries it's the <symbol>
#     on the call line. Both are handled.
set -euo pipefail

f="${1:?usage: $0 <binary or .o> [function regex]}"
fn="${2:-.}"
command -v objdump >/dev/null 2>&1 || { echo "error: objdump not found (apt install binutils)" >&2; exit 127; }
[ -e "$f" ] || { echo "error: no such file: $f" >&2; exit 1; }

PAT='operator new|operator delete|(<|[[:space:]])(malloc|calloc|realloc|free|posix_memalign|aligned_alloc|syscall|write|read|send|recv|sendmsg|recvmsg|sendto|recvfrom|epoll_wait|epoll_pwait|poll|select|nanosleep|clock_nanosleep|usleep|sched_yield|getaddrinfo|mmap|munmap|msync|fsync)(@plt|-0x[0-9a-f]+|>)|__cxa_throw|__cxa_allocate_exception|__cxa_begin_catch|__cxa_guard_acquire|__tls_get_addr|pthread_mutex_|pthread_cond_|pthread_rwlock_|pthread_spin_|__divti3|__udivti3|__modti3|__umodti3|std::function|_M_invoke|std::basic_ostream|std::locale|std::__cxx11::basic_string.*(_M_create|_M_mutate|_M_replace|_M_append)|std::vector.*_M_realloc_insert|_Hashtable.*_M_rehash'

raw=$(objdump -d -C --no-show-raw-insn -r "$f" 2>/dev/null) || { echo "objdump failed: $f" >&2; exit 1; }

hits=$(printf '%s\n' "$raw" | awk -v pat="$PAT" -v fn="$fn" '
    /^[0-9a-f]+ <.*>:$/ {
        cur=$0; sub(/^[0-9a-f]+ </,"",cur); sub(/>:$/,"",cur)
        show = (cur ~ fn); next
    }
    !show { next }
    {
        # Match against the call target only, not the whole line (the current function name may
        # itself contain e.g. "_M_realloc_insert"): "<sym>" on call/jmp lines, the last field on relocations.
        tgt=""
        if ($0 ~ /R_(X86_64|AARCH64)_/) { tgt=$0; sub(/^.*R_[A-Z0-9_]+[[:space:]]+/, "", tgt) }
        else if ($0 ~ /[[:space:]](call|jmp)[a-z]*[[:space:]]/ && match($0, /<[^>]*>$/)) {
            tgt=substr($0, RSTART, RLENGTH)
            if (tgt ~ /\+0x[0-9a-f]+>$/) next      # jump inside a function, not a call
        }
        if (tgt != "" && tolower(" " tgt) ~ tolower(pat)) {
            line=$0; gsub(/^[ \t]+/,"",line); gsub(/[ \t]+/," ",line)
            print cur "\t" line
        }
    }
')

if [ -z "$hits" ]; then
    echo "no matching call sites (filter: $fn). Inlined and indirect calls are not detected."
    exit 0
fi

echo "==== hits (function -> call site) ===="
printf '%s\n' "$hits" | sort -u
echo
echo "==== count by call target ===="
printf '%s\n' "$hits" | sort -u | awk -F'\t' '{print $2}' \
    | sed -E 's/^[0-9a-f]+:[[:space:]]*//; s/^(call|jmp)[a-z]*[[:space:]]+[0-9a-f]+[[:space:]]*//; s/^R_[A-Z0-9_]+[[:space:]]+//' \
    | sort | uniq -c | sort -rn | head -30
