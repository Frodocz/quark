// AllocGuard.h: for tests only. Counts calls to the global operator new so a test can assert
// "zero allocation on the hot path".
//
// Usage:
//   - Include it in exactly ONE translation unit of a test executable. (A replacement global
//     allocation function can't be inline, so including it twice gives duplicate definitions.)
//   - Don't use it with ASan/TSan configs (it would bypass their heap interception); use it in the
//     rel / noexc test builds.
//   - Only allocations that go through operator new are counted (operator new[] and the nothrow
//     forms forward to it by default). Code that calls malloc directly (some C libraries, e.g. the
//     TLS backend) isn't counted; combine with scripts/hotpath_symbols.sh and ltrace/heaptrack.
//
//   quark_review::NoAllocScope scope;
//   hot_path_under_test();
//   EXPECT_EQ(scope.count(), 0u);
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace quark_review {

inline std::atomic<std::uint64_t> g_alloc_count{0};
inline std::atomic<bool> g_alloc_armed{false};

inline void note_alloc() noexcept {
    if (g_alloc_armed.load(std::memory_order_relaxed)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    }
}

[[noreturn]] inline void out_of_memory() {
#if defined(__cpp_exceptions)
    throw std::bad_alloc{};
#else
    std::abort();
#endif
}

// Resets and starts counting on entry, stops on exit. count() returns the number of allocations
// made inside the scope, by all threads.
class NoAllocScope {
public:
    NoAllocScope() noexcept {
        g_alloc_count.store(0, std::memory_order_relaxed);
        g_alloc_armed.store(true, std::memory_order_release);
    }
    ~NoAllocScope() { g_alloc_armed.store(false, std::memory_order_release); }
    NoAllocScope(const NoAllocScope&) = delete;
    NoAllocScope& operator=(const NoAllocScope&) = delete;

    [[nodiscard]] std::uint64_t count() const noexcept {
        return g_alloc_count.load(std::memory_order_relaxed);
    }
};

}  // namespace quark_review

void* operator new(std::size_t n) {
    quark_review::note_alloc();
    if (void* p = std::malloc(n ? n : 1)) return p;
    quark_review::out_of_memory();
}

void* operator new(std::size_t n, std::align_val_t al) {
    quark_review::note_alloc();
    const auto a = static_cast<std::size_t>(al);
    const std::size_t rounded = ((n ? n : 1) + a - 1) / a * a;  // aligned_alloc needs size to be a multiple of alignment
    if (void* p = std::aligned_alloc(a, rounded)) return p;
    quark_review::out_of_memory();
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
