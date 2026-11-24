# parsing checklist (JsonParser / crypto / encodings)

Scope: `utils/JsonParser.h`, `crypto/*`, `src/crypto/*`, `utils/Crc32c.h`, `utils/HwEndian.h`,
`net/Url.h`. QuarkConfig (toml) belongs to `runtime`.
Consumers: `JsonParser::parse_in_place` on **every** MD message (Binance and Bitget JSON) and every
order ack/fill (`tradeserver EventParser`, `JsonParser json_{1<<16}`); prices are parsed as
`double`; trading catches `simdjson::simdjson_error`. `HmacSha256::sign_hex` runs on **every
Binance order/cancel/modify**, and `sign_base64` at Bitget login/REST. Ed25519Signer, Base64Encoding
and HwEndian aren't used by trading (latent). Crc32c is used through Journal.

## Checks
1. simdjson integration: the padded-buffer requirement (does `parse_in_place` own or need
   `SIMDJSON_PADDING` bytes after the message inside the WS receive buffer?); lifetime of
   document/value, and whether a string_view dangles; parser reuse and preallocation (zero
   allocation on the hot path; what happens when a message is larger than the capacity?); the parser
   is not thread-safe (thread_local or a documented constraint); what is thrown vs returned as an
   error code, and consistency with trading's `try/catch (simdjson_error)`; behaviour for duplicate
   keys, depth, oversized input, bad escapes, number formats (-0, exponents, huge integers); can
   numbers be parsed straight into fixed point without going through double?
2. Cryptography: does each sign/HMAC allocate or rebuild a context (can it be reused)? Is every
   return value checked? Key lifetime and zeroing (`OPENSSL_cleanse`); constant-time comparison;
   output buffer length checks; does the Base64 variant (URL-safe/padding) match what the exchange
   expects? AWS-LC vs OpenSSL differences. HMAC is most of the 2 µs `Router::insert` cost, so
   per-call cycles matter (benchmarks/crypto/CryptoBench.cpp).
3. Crc32c: do the hardware and software paths give the same result? Alignment and tail handling;
   are ISA compile flags or runtime dispatch right (no SIGILL on CPUs without the instruction; see LL-001)?
4. HwEndian: aligned access, strict aliasing (should use memcpy/`std::bit_cast`), reads in non-native byte order.
5. Url: parsing of host/port/path/query, IPv6 literals, and no generic helper names leaking into the
   consumer namespace (LL-004).

## Verification
- Known-answer tests: HMAC-SHA256 (RFC 4231), Ed25519 (RFC 8032), Base64 (RFC 4648),
  CRC32C("123456789") = 0xE3069283.
- libFuzzer for JSON/Url (ASan+UBSan, needs clang). Seed the corpus with sanitised real exchange
  messages (`tests/utils/Binance_LinearSwap_BTCUSDT_depth.json`, trading's `tests/data/*`).
- Benchmark: cycles and allocation count per sign/parse; assert 0 for paths that should be allocation-free.
