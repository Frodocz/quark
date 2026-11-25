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
