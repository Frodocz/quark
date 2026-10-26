#pragma once

#include "quark/Attributes.h"
#include "quark/QuarkError.h"
#include "quark/QuarkLog.h"
#include "quark/Sanitizers.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>

QUARK_BEGIN_NAMESPACE

/// One block of memory reserved up front and handed out by a bump allocator, for pools and other
/// long-lived structures that must not page-fault, be swapped out or miss the TLB on the hot path.
///
/// - Huge pages: random access over a working set of a few MiB or more misses the TLB on 4 KiB
///   pages; on 2 MiB pages it doesn't (~15-30% faster object churn at 10-100 MiB).
/// - The whole block is written once at construction (no page fault later) and mlock'ed (never
///   swapped out). Neither changes steady-state speed; both remove latency outliers.
/// - allocate() is a pointer bump; memory is released only when the arena is destroyed.
/// - Single-threaded. Everything allocated from it must be done with before it is destroyed.
/// - ASan builds poison the unallocated remainder, so overruns past an allocation are reported.
class MemoryArena {
public:
    static constexpr std::size_t kHugePageSize = 2 * 1024 * 1024;

    enum class HugePages : uint8_t {
        Never,   ///< 4 KiB pages only
        Auto,    ///< huge pages when the arena spans at least one huge page, else 4 KiB pages
        Always,  ///< huge pages whatever the size (rounds the arena up to 2 MiB)
    };

    /// What the arena ended up backed by, in order of preference.
    enum class PageKind : uint8_t {
        HugeTlb,          ///< reserved huge pages (MAP_HUGETLB, see /proc/sys/vm/nr_hugepages)
        TransparentHuge,  ///< 4 KiB mapping advised MADV_HUGEPAGE: huge pages when the kernel has them
        Normal,           ///< 4 KiB pages
    };

    struct Options {
        HugePages huge_pages{HugePages::Auto};
        bool lock{true};  ///< mlock; failure (e.g. RLIMIT_MEMLOCK) is logged, not fatal
    };

    /// Reserves at least `capacity` bytes (rounded up to whole pages). Throws QuarkError if
    /// `capacity` is 0 or the memory can't be mapped.
    explicit MemoryArena(std::size_t capacity) : MemoryArena(capacity, Options{}) {}

    MemoryArena(std::size_t capacity, const Options& options) {
        if (capacity == 0) {
            QUARK_THROW(QuarkError{"MemoryArena: zero capacity"});
        }
        const bool want_huge = options.huge_pages == HugePages::Always ||
                               (options.huge_pages == HugePages::Auto && capacity >= kHugePageSize);
        map(capacity, want_huge);

        // fault every page in now rather than on first use
        const std::size_t step = page_kind_ == PageKind::HugeTlb ? kHugePageSize : page_size_;
        for (std::size_t off = 0; off < size_; off += step) base_[off] = 0;
        if (options.lock) {
            locked_ = ::mlock(base_, size_) == 0;
            if (!locked_) {
                QUARK_LOG_WARN("MemoryArena: mlock of {} bytes failed (errno={}), pages may be swapped out",
                               size_, errno);
            }
        }
        (void)::madvise(base_, size_, MADV_DONTDUMP);
        QUARK_ASAN_POISON(base_, size_);
    }

    ~MemoryArena() {
        QUARK_ASAN_UNPOISON(base_, size_);
        (void)::munmap(base_, size_);
    }

    MemoryArena(const MemoryArena&) = delete;
    MemoryArena& operator=(const MemoryArena&) = delete;

    /// Returns `bytes` (> 0) bytes aligned to `align` (a power of two, at most the page size), or
    /// nullptr if the arena doesn't have that much left.
    [[nodiscard]] QUARK_ALWAYS_INLINE void* allocate(std::size_t bytes,
                                                     std::size_t align = alignof(std::max_align_t)) noexcept {
        assert(bytes > 0 && align != 0 && (align & (align - 1)) == 0 && align <= page_size_);
        if (bytes == 0) bytes = 1;
        const std::size_t start = (used_ + align - 1) & ~(align - 1);
        if (start > size_ || bytes > size_ - start) [[unlikely]] return nullptr;
        used_ = start + bytes;
        void* p = base_ + start;
        QUARK_ASAN_UNPOISON(p, bytes);
        return p;
    }

    /// Whether `p` points into this arena.
    [[nodiscard]] bool contains(const void* p) const noexcept {
        const auto* b = static_cast<const uint8_t*>(p);
        return b >= base_ && b < base_ + size_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return size_; }
    [[nodiscard]] std::size_t used() const noexcept { return used_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return size_ - used_; }
    [[nodiscard]] PageKind page_kind() const noexcept { return page_kind_; }
    [[nodiscard]] bool locked() const noexcept { return locked_; }

private:
    uint8_t* base_{nullptr};
    std::size_t size_{0};
    std::size_t used_{0};
    std::size_t page_size_{static_cast<std::size_t>(::sysconf(_SC_PAGESIZE))};
    PageKind page_kind_{PageKind::Normal};
    bool locked_{false};

    QUARK_NOINLINE QUARK_ATTRIBUTE_COLD void map(std::size_t capacity, bool want_huge) {
        constexpr int kProt = PROT_READ | PROT_WRITE;
        constexpr int kFlags = MAP_PRIVATE | MAP_ANONYMOUS;
        if (want_huge) {
            size_ = round_up(capacity, kHugePageSize);
            void* p = ::mmap(nullptr, size_, kProt, kFlags | MAP_HUGETLB, -1, 0);
            if (p != MAP_FAILED) {
                base_ = static_cast<uint8_t*>(p);
                page_kind_ = PageKind::HugeTlb;
                return;
            }
            // no reserved huge pages: ask for transparent ones. Advised before the prefault, the
            // pages are huge from the first touch, so there is nothing for khugepaged to collapse later
            p = ::mmap(nullptr, size_, kProt, kFlags, -1, 0);
            if (p != MAP_FAILED) {
                base_ = static_cast<uint8_t*>(p);
                page_kind_ = ::madvise(base_, size_, MADV_HUGEPAGE) == 0 ? PageKind::TransparentHuge
                                                                         : PageKind::Normal;
                return;
            }
        } else {
            size_ = round_up(capacity, page_size_);
            void* p = ::mmap(nullptr, size_, kProt, kFlags, -1, 0);
            if (p != MAP_FAILED) {
                base_ = static_cast<uint8_t*>(p);
                // small arenas stay on 4 KiB pages: a 2 MiB page would mostly be wasted
                (void)::madvise(base_, size_, MADV_NOHUGEPAGE);
                page_kind_ = PageKind::Normal;
                return;
            }
        }
        QUARK_THROW(QuarkError{"MemoryArena: mmap failed"});
    }

    static std::size_t round_up(std::size_t n, std::size_t multiple) {
        if (n > std::numeric_limits<std::size_t>::max() - multiple) {
            QUARK_THROW(QuarkError{"MemoryArena: capacity too large"});
        }
        return (n + multiple - 1) / multiple * multiple;
    }
};

QUARK_END_NAMESPACE
