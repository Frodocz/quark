#pragma once

#include "quark/Attributes.h"

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

QUARK_BEGIN_NAMESPACE

/// Remembers 64-bit keys (e.g. message hashes) for `window_ns`: first() says whether a key is new.
/// For redundant feeds, where the same message arrives over several connections. Fixed capacity,
/// open addressing; expired entries are reused in place, nothing is allocated after construction.
/// Size `capacity` for a few times the keys one window holds: a full neighbourhood evicts its
/// oldest entry, which can only let a very old duplicate through.
class TimeScopedDedup {
public:
    TimeScopedDedup(int64_t window_ns, std::size_t capacity)
        : window_ns_{window_ns}, slots_(std::bit_ceil(capacity < 16 ? std::size_t{16} : capacity)), mask_{slots_.size() - 1} {
        assert(window_ns > 0);
    }

    struct Seen {
        bool first;          ///< not seen within the window
        int64_t first_ns;    ///< when it was first seen (now_ns if it's new)
    };

    /// Records `key` at `now_ns` unless it's already known within the window.
    Seen first(uint64_t key, int64_t now_ns) noexcept {
        key |= key == 0;  // 0 marks an empty slot
        std::size_t free = kNone, oldest = kNone;
        for (std::size_t i = 0, p = mix(key) & mask_; i < kMaxProbes; ++i, p = (p + 1) & mask_) {
            Slot& s = slots_[p];
            if (s.key == key && now_ns - s.ts < window_ns_) return {false, s.ts};
            if (s.key == 0) {  // never used: the key isn't further along
                if (free == kNone) free = p;
                break;
            }
            if (free == kNone && (s.key == key || now_ns - s.ts >= window_ns_)) free = p;  // expired: reusable
            if (oldest == kNone || s.ts < slots_[oldest].ts) oldest = p;
        }
        slots_[free != kNone ? free : oldest] = {key, now_ns};
        return {true, now_ns};
    }

    [[nodiscard]] int64_t window_ns() const noexcept { return window_ns_; }

private:
    static constexpr std::size_t kMaxProbes = 32;
    static constexpr std::size_t kNone = ~std::size_t{0};
    struct Slot {
        uint64_t key = 0;
        int64_t ts = 0;
    };
    int64_t window_ns_;
    std::vector<Slot> slots_;
    std::size_t mask_;

    static uint64_t mix(uint64_t x) noexcept {  // keys may be weak hashes: spread them over the table
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        return x ^ (x >> 33);
    }
};

QUARK_END_NAMESPACE
