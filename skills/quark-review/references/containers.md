# containers checklist (SPSC / FullDuplexShm / ShmDirectory / Journal / pools / arena / TimerQueue / InplaceFunction)

Scope: `third_party/SPSC_Queue` (SPSCQueue/OPT, SPSCVarQueue/OPT), `utils/CommandQueue`,
`CircularBuffer`, `FullDuplexShm`, `ShmDirectory`, `Journal`, `MemoryArena`, `SimpleObjectPool`,
`InplaceFunction`, `TimerQueue`, `MMapFile` (+ `src/utils/MMapFile.cpp`), `net/BufferPool`, `net/IOBuffer`.
Consumers: `FullDuplexShm<64 MiB>` is the **cross-process ABI** between alphaframe `TsClient` and
tradeserver `AlgoSession`, and must stay byte-compatible with the legacy binaries; `ShmDirectory`
session discovery and flock liveness on the tradeserver SessionThread; `Journal` appended on the
tradeserver loop thread, `sync()` on the SessionThread; `SimpleObjectPool<Order>` and
`InplaceFunction` callbacks in alphaframe; `TimerQueue` via `EventLoop::timers()`.
CommandQueue, CircularBuffer, MemoryArena and standalone SPSCQueue aren't used by trading (latent).

## Checks
1. SPSC contract: is single-producer/single-consumer enforced by type or assertion? Are head/tail
   and the cached indices on separate cache lines? For each atomic, write down the release/acquire
   pairing that justifies its `memory_order`. Check full/empty detection and wrap-around;
   variable-length padding and messages that cross the end; leftover elements on destruction;
   construction/destruction of non-trivial `T`.
   Note: CMake warns that TSan with non-Clang compilers can report false positives. Use clang
   (`build_matrix.sh tsan` picks clang++ 14 when installed). Tell tool false positives apart from
   real races and give the criterion you used.
   **SPSCVarQueue barriers (re-check every review that touches SHM).** `SPSCVarQueue.h:55-112`
   (vendored, unmodified) uses plain non-atomic `write_idx`/`read_idx`/block headers with
   `asm volatile("" : "=m"(x) ...)` / `("" : : "m"(x))` compiler barriers and **no `"memory"`
   clobber**, relying on x86-TSO for ordering between CPUs. Questions:
   (a) Does each barrier constrain every location it has to order (payload bytes written through
   `blk` vs the publishing store to `write_idx`)? An `"m"` operand covers only the named object, so
   other stores may move across it.
   (b) Is it still correct under LTO and `-march=native` (the trading build), where more inlining
   lets the compiler see through the call boundaries that hid reorderings before?
   (c) What exactly does the cross-process claim cover? Trading's `IMPLEMENTATION_SUMMARY.md` says
   "production is unaffected: the writer is another process". That's true for the compiler's view
   of the *peer's* stores, not for ordering inside one process's own writes. History: `TestShmLayout` failed
   only in Release because `"=m"` let GCC drop the test's own stores (fixed in the test, f6f28b1).
   Evidence for (a)/(b): read the disassembly of `FullDuplexShm` write/read paths in an `-O3 -march=native -flto`
   build, plus a two-process stress test (writer and reader on different cores, sequence-checked payloads).
2. Shared memory and files: `static_assert(is_always_lock_free)` and `is_trivially_copyable`; stable
   layout (magic/version, no pointers, endianness); the 64 MiB + 256 B size check (`TestShmLayout`);
   recovery when the peer crashes or dies mid-write; flock liveness semantics (`LOCK_EX` by the
   writer, `LOCK_SH|LOCK_NB` probe); permissions and cleanup; mmap alignment and pre-faulting
   (MAP_POPULATE/mlock) so the hot path doesn't page-fault; SIGBUS if a mapped file is truncated externally.
3. Journal: write atomicity (header written last, or covered by the CRC?); CRC over seq + payload;
   torn-tail recovery and `torn_bytes()`; behaviour when full (append refused, the caller rotates);
   **`sync()` from another thread while the loop appends** (the mapping is shared, so msync racing
   with memcpy is benign only if nothing else touches the bookkeeping; check every member that
   `sync()` reads); persisted format stability across versions; replay determinism.
4. Pools and arena: alignment; behaviour when exhausted (is refusal or fallback deterministic?);
   double free and freeing from another thread (the double-free check exists only `#ifndef NDEBUG`);
   reset semantics and destruction; fragmentation; whether the ASan poisoning (Sanitizers.h) covers
   both allocation and free.
5. InplaceFunction: does a capture that is too large fail at compile time? Copy/move/empty-call
   semantics, and how they fit the exception policy (`QUARK_THROW` on an empty call?).
6. TimerQueue (a `std::vector` heap + `unique_ptr<Node[]>` chunks, `reserve()` is COLD): does growth
   happen on the hot path when more timers are pending than were reserved? Does lazy cancellation
   bloat the heap? Adding or cancelling from inside a callback (poll is not re-entrant; the assert
   is debug-only); ordering of timers due at the same time; a cap on work per poll; clock steps and 64-bit overflow.
7. False sharing: quark's own code has no cache-line constant or `alignas(64)`; only the vendored
   SPSC queues align. Check shared structs written by two threads (Journal state that `sync()` reads,
   LatencyRecorder, stats) and don't rely on `std::hardware_destructive_interference_size` (GCC
   warns about its ABI).

## Verification
- Model tests against `std::deque` / `std::multimap` with random operation sequences; full, empty,
  wrap-around and batch edges.
- TSan stress with random delay injection; crash injection for shared memory and Journal (truncate
  mid-write); a test that pairs an old-layout writer with a quark reader.
- Zero allocation on the hot path: add `templates/AllocGuard.h` to a test and assert the allocation
  count is 0 once the hot phase starts (only in non-ASan configs).
