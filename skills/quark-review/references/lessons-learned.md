# lessons-learned (defect patterns actually found)

Read this before every review and check these patterns first. Each entry must come from a real
defect, not a guess. Entries are added only after the user confirms them. At the end of a review
the reviewer may propose candidates but must not edit this file.

## Entry format

### LL-NNN One-line title
- Module / file:
- Symptom and trigger:
- Root cause:
- How to check (command, grep pattern, test method):
- Fix commit / date:

## Entries

### LL-001 Internal headers must open namespaces with QUARK_BEGIN_NAMESPACE
- Module / file: `src/net/TlsInternal.h`, `src/crypto/OpenSslError.h`
- Symptom and trigger: clang 14 builds failed to link the TLS code (undefined
  `quark::net::detail::openssl_error_string()` / `on_new_tls_session()`); GCC builds linked fine.
- Root cause: `namespace quark::net::detail {` written by hand instead of `QUARK_BEGIN_NAMESPACE` +
  `namespace net::detail`. GCC treats it as reopening the inline `quark::v1::net::detail`; clang 14 opens
  a separate unversioned `quark::net::detail`, so declarations and definitions end up in different namespaces.
  Even when it links (inline functions), the symbol escapes the `v1` version guard.
- How to check: `grep -rn "^namespace quark" include src` (only `quark::fmtlib` in QuarkFormat.h is
  intentional); ctest `quark_test_symbols_versioned` fails on any `quark::` symbol outside `quark::v1`;
  build once with clang.
- Fix commit / date: 4371aba (2026-11-25)

### LL-002 "Success" returned after a callback already reported the failure
- Module / file: `net/HttpClient.h` (`request()`), and `HttpPool` through it
- Symptom and trigger: the peer reset an idle keep-alive connection; the next `request()` failed while
  sending, called `on_error` from inside, then returned `Submit::Ok`. Trading's callers set
  "busy" on `Ok` after their `on_error` had already cleared it: REST snapshots (alphaframe) and REST
  queries (tradeserver) stopped for the life of the process.
- Root cause: a synchronous failure path (`fail()` → `on_error_()`) reachable from inside an API call
  whose return value the caller trusts. The two channels disagreed about what happened.
- How to check: for every call that can tear down (`fail()`, `teardown()`, write/flush errors), see
  whether it is reachable from a public call that then returns success. An accepted request must get
  exactly one later callback; failures inside the call are deferred (a zero-delay timer) or returned.
  Test: make the failure synchronous (RST before the call, without stepping the loop) and check that
  no callback runs inside the call.
- Fix commit / date: NET-01 (2026-11-25)

### LL-003 Recursion on network input
- Module / file: `net/HttpCodec.h` (`parse_http_response`)
- Symptom and trigger: one stack frame per 1xx interim response; ~58k "100 Continue" responses in a
  1 MiB receive buffer (the tradeserver's size) overflowed the stack: SIGSEGV.
- Root cause: a parser that calls itself once per repeated element of untrusted input, with work after
  the call (no tail call), so depth grows with the input.
- How to check: `grep` parsers for self-calls; replace with a loop and a cap. Fuzz with inputs as large
  as the production buffers (`-max_len` of 4 KiB missed this one); add a test with a buffer-filling
  flood of the repeated element.
- Fix commit / date: NET-02 (2026-11-25)
