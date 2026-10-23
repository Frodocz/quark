#pragma once

#include "quark/Attributes.h"
#include "quark/Sanitizers.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef NDEBUG
    #include <unordered_set>
#endif

QUARK_BEGIN_NAMESPACE

/// Single-threaded pool of fixed-type objects.
///
/// - Storage is a list of chunks that are never moved or released before the pool
///   dies, so pointers returned by alloc() stay valid across growth.
/// - Free slots are kept on a separate LIFO stack of pointers, so free() never writes to
///   the (often cold) object memory, and the most recently freed, cache-hot slot is
///   reused first. The stack is reserved to full capacity and never allocates.
/// - Slots are raw storage: alloc() constructs T in place, free() destroys it, and
///   there is no double construction/destruction.
/// - Chunks are zero-filled when allocated, so their pages are faulted in up front,
///   not on the hot path. Growth when exhausted is a cold path: size
///   prealloc_sz for the peak working set.
/// - Debug builds catch double free and foreign pointers; ASan builds poison free
///   slots so use-after-free is reported.
///
/// Objects still allocated when the pool is destroyed are not destroyed (their memory
/// is released): free everything first unless T is trivially destructible.
template <class T>
    requires std::is_nothrow_destructible_v<T>
class SimpleObjectPool {
public:
    explicit SimpleObjectPool(size_t prealloc_sz = 4096, size_t batch_sz = 128) : batch_sz_{batch_sz} {
        assert(batch_sz > 0);
        if (prealloc_sz > 0) add_chunk(prealloc_sz);
    }

    ~SimpleObjectPool() {
        assert((std::is_trivially_destructible_v<T> || used_ == 0) &&
               "SimpleObjectPool destroyed with live objects that need destruction");
        for (const Chunk& c : chunks_) {
            QUARK_ASAN_UNPOISON(c.slots, c.count * sizeof(Slot));
            ::operator delete(c.slots, std::align_val_t{kChunkAlignment});
        }
    }

    SimpleObjectPool(const SimpleObjectPool&) = delete;
    SimpleObjectPool& operator=(const SimpleObjectPool&) = delete;

    /// Constructs a T from `args` in a free slot. Value-initializes when called without
    /// arguments (PODs come back zeroed).
    template <class... Args>
        requires std::is_constructible_v<T, Args...>
    [[nodiscard]] T* alloc(Args&&... args) {
        if (free_.empty()) [[unlikely]] grow();

        Slot* s = free_.back();
        free_.pop_back();
        QUARK_ASAN_UNPOISON(s, sizeof(Slot));

        if constexpr (std::is_nothrow_constructible_v<T, Args...>) {
            return finish_alloc(::new (static_cast<void*>(s)) T(std::forward<Args>(args)...));
        } else {
            // give the slot back if T's constructor throws
            struct Guard {
                SimpleObjectPool* pool;
                Slot* slot;
                ~Guard() {
                    if (slot) pool->push_free(slot);
                }
            } guard{this, s};
            T* obj = ::new (static_cast<void*>(s)) T(std::forward<Args>(args)...);
            guard.slot = nullptr;
            return finish_alloc(obj);
        }
    }

    /// Destroys `obj` and returns its slot to the pool. free(nullptr) is a no-op.
    void free(T* obj) noexcept {
        if (obj == nullptr) return;
#ifndef NDEBUG
        const bool owned = live_.erase(obj) == 1;
        assert(owned && "SimpleObjectPool::free(): double free or pointer not from this pool");
#endif
        obj->~T();
        --used_;
        push_free(obj);
    }

    size_t get_capacity() const noexcept { return capacity_; }

    size_t get_size_avail() const noexcept { return capacity_ - used_; }

    size_t get_size_used() const noexcept { return used_; }

private:
    struct Slot {
        alignas(T) std::byte storage[sizeof(T)];
    };

    struct Chunk {
        Slot* slots;
        size_t count;
    };

    // chunks start on a cache line so slot layout relative to cache lines is predictable
    static constexpr size_t kChunkAlignment = alignof(Slot) > 64 ? alignof(Slot) : 64;

    std::vector<Slot*> free_;
    size_t used_{0};
    size_t capacity_{0};
    const size_t batch_sz_;
    std::vector<Chunk> chunks_;
#ifndef NDEBUG
    std::unordered_set<const T*> live_;
#endif

    T* finish_alloc(T* obj) {
        ++used_;
#ifndef NDEBUG
        live_.insert(obj);
#endif
        return obj;
    }

    // takes void*: obj sits at the start of its Slot, so it is Slot-aligned even if T is
    // packed, but gcc's -Waddress-of-packed-member cannot see that through a T* -> Slot* cast
    void push_free(void* p) noexcept {
        Slot* s = static_cast<Slot*>(p);
        free_.push_back(s);  // never reallocates: reserved to capacity_
        QUARK_ASAN_POISON(s, sizeof(Slot));
    }

    QUARK_NOINLINE QUARK_ATTRIBUTE_COLD void grow() { add_chunk(batch_sz_); }

    void add_chunk(size_t count) {
        assert(count <= std::numeric_limits<size_t>::max() / sizeof(Slot));
        // reserve first so nothing can throw once the chunk is allocated
        chunks_.reserve(chunks_.size() + 1);
        free_.reserve(capacity_ + count);
        const size_t bytes = count * sizeof(Slot);
        auto* slots = static_cast<Slot*>(::operator new(bytes, std::align_val_t{kChunkAlignment}));
        std::memset(static_cast<void*>(slots), 0, bytes);  // prefault
        chunks_.push_back(Chunk{slots, count});
        // push back to front so allocations walk the chunk in address order
        for (size_t i = count; i-- > 0;) {
            push_free(&slots[i]);
        }
        capacity_ += count;
    }
};

/// Debug/diagnostic alternative: one heap allocation per object, tracked in a set.
/// Every object is a separate allocation, so ASan/valgrind can catch overflows between
/// neighbouring objects, which SimpleObjectPool cannot.
template <class T>
    requires std::is_nothrow_destructible_v<T>
class HeapMemoryPool {
public:
    explicit HeapMemoryPool(size_t = 0, size_t = 0) {}

    ~HeapMemoryPool() {
        for (auto* obj : alive_objects_) {
            delete obj;
        }
    }

    HeapMemoryPool(const HeapMemoryPool&) = delete;
    HeapMemoryPool& operator=(const HeapMemoryPool&) = delete;

    template <class... Args>
        requires std::is_constructible_v<T, Args...>
    [[nodiscard]] T* alloc(Args&&... args) {
        auto* ret = new T(std::forward<Args>(args)...);
        alive_objects_.insert(ret);
        return ret;
    }

    void free(T* obj) noexcept {
        if (obj == nullptr) return;
        const bool owned = alive_objects_.erase(obj) == 1;
        assert(owned && "HeapMemoryPool::free(): double free or pointer not from this pool");
        if (owned) delete obj;
    }

    size_t get_capacity() const noexcept { return std::numeric_limits<size_t>::max(); }

    size_t get_size_avail() const noexcept { return std::numeric_limits<size_t>::max(); }

    size_t get_size_used() const noexcept { return alive_objects_.size(); }

private:
    std::set<T*> alive_objects_;
};

// SimpleObjectPool does its own double-free checks in debug builds, so debug and release
// run the same allocator.
template <class T>
using QuarkObjectPool = SimpleObjectPool<T>;

QUARK_END_NAMESPACE

