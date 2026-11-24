# net checklist (EventLoop / Poller / Tcp / Tls / Http / WebSocket / WsGroup / Buffer / F-Stack)

Scope: `include/quark/net/*`, `src/net/*`, `src/TlsBackend.h`.
Consumers (all hot, on one busy-poll loop thread per process): MD over
`WsGroup<TlsStream>` (`alphaframe/src/md/Feed.cpp`); Binance WS-API orders with
`WsGroup::send_best` (`tradeserver/src/BinanceUsdm.cpp`); Bitget `WsSession::send_text`; REST
snapshots and queries with `HttpClient<TlsStream>` (warm); `Resolver` refresh thread and
`EndpointProvider` on reconnect. TcpListener/HttpPool/F-Stack aren't used by trading (latent).

## Checks
1. Event loop: callbacks that add/remove fds or timers, or destroy their own object (re-entrancy and
   use-after-free); stale events that arrive after an fd is closed; races on fd-number reuse;
   correct LT/ET handling; a per-iteration budget so one fd or a burst of timers can't starve the
   rest; the `step()` "did work" return value that drives `OnIdle()` in trading.
2. Socket I/O: partial read/write and the write buffer; EAGAIN/EINTR/EPIPE; MSG_NOSIGNAL; half-close;
   connect timeout; whether socket options (TCP_NODELAY, buffer sizes, busy poll) are set and configurable.
3. TLS (AWS-LC/OpenSSL backends): the WANT_READ/WANT_WRITE state machine of non-blocking
   SSL_read/SSL_write; shutdown sequence; certificate and host-name verification and SNI on by
   default; session resumption; clearing the error queue (when `ERR_clear_error` runs; it is
   thread-local); differences between the two backends; allocations per message.
4. Buffers: growth policy and cap; back-pressure; whether a span/string_view handed to a callback
   (e.g. `WsMessage` passed into `JsonParser::parse_in_place`) dangles after the next recv or is
   modified in place; deterministic behaviour when BufferPool runs out; `WsSendStatus::Dropped` semantics.
5. Protocol state machines: HTTP (chunked, Content-Length, header injection, oversized headers);
   WebSocket (masking, fragmentation, control frames, ping/pong including text ping/pong and RTT,
   close codes, max payload, UTF-8); handshake validation (a 429/418 from a rate-limiting exchange
   must fail the attempt cleanly). Any malformed input must fail safely, never crash or loop forever.
6. WsGroup: dedup correctness across N sessions (no drops, no duplicates when sessions reconnect);
   whether `send_best` picks a live session and what happens when none is up (the order must fail
   loudly, not vanish); memory growth in the dedup window.
7. Connection management: ExpBackoff jitter, cap and overflow; AliveMonitor and recv-gap checks on a
   monotonic clock; `Resolver` (getaddrinfo blocks) kept off the hot loop, and the handover between
   the resolver thread and the loop is race-free; reconnect storms.
8. F-Stack (only the `QUARK_USE_FSTACK` path): does the ff_* thread model behave like the
   kernel-socket path, and does the abstraction leak?

## Verification
- Loopback integration tests (`tests/net/TestServers.h`) with fault injection: random
  fragmentation, delay, RST, peer half-close, slow consumer.
- Run all net tests in the `asan` and `tsan` configs of build_matrix.sh. Use `strace -c -f` to count
  syscalls per message and assert an upper bound.
- Fuzz malformed packets (libFuzzer + ASan/UBSan, needs clang): HttpCodec, WsFrame, WsHandshake, Url.
