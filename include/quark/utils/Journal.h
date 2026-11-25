#pragma once

#include "quark/Attributes.h"
#include "quark/QuarkError.h"
#include "quark/QuarkFormat.h"
#include "quark/utils/Crc32c.h"
#include "quark/utils/MMapFile.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>

#include <sys/mman.h>
#include <unistd.h>

#ifndef MADV_POPULATE_WRITE
    #define MADV_POPULATE_WRITE 23  // Linux 5.14+; older kernel headers lack it
#endif

QUARK_BEGIN_NAMESPACE

/// Append-only record log in a preallocated, memory-mapped file: an append is a memcpy, no syscall.
///
/// Each record is `Header{size, crc, seq}` + payload, padded to 8 bytes; seq counts from 1 per file.
/// Opening an existing file scans it and resumes after the last valid record: a record cut short by
/// a crash fails its CRC and ends the scan (`torn_bytes()` reports what follows it). A full journal
/// refuses appends: the caller starts a new file (e.g. after writing a snapshot).
///
/// A crashed process loses nothing (the page cache keeps the mapping); call sync() from a
/// housekeeping thread to also survive a power loss. Everything else is single-threaded.
///
/// Page faults: the pages of a shared file mapping are write-protected until their first write, also
/// when mlock'ed, and again each time they are written back (sync(), or the kernel's flusher). An
/// append to such a page takes a fault (~3 us, sometimes ms). So the pages from the tail to
/// `prefault_bytes` ahead are write-faulted by the constructor and again by every sync() or
/// prefault(): call one of them from a housekeeping thread often enough that the appends between
/// two calls fit in `prefault_bytes`.
class Journal {
public:
    struct Header {
        uint32_t size;  ///< payload bytes; written last, so a record is visible only once complete
        uint32_t crc;   ///< crc32c of seq and payload
        uint64_t seq;
    };
    static constexpr std::size_t kAlign = 8;

    struct Options {
        std::string path;
        std::size_t capacity{256u << 20};  ///< file size for a new journal
        bool lock{true};                   ///< mlock the file: its pages are never swapped out
        std::size_t prefault_bytes{4u << 20};  ///< bytes ahead of the tail kept write-faulted, see prefault()
    };

    explicit Journal(const Options& options) {
        const bool exists = std::filesystem::exists(options.path);
        file_.initialize({.filename = options.path,
                          .writable = true,
                          .lock = options.lock,
                          .create = true,
                          .mode = 0644,
                          .size = exists ? 0 : options.capacity});
        if (file_.size() < sizeof(Header)) {
            QUARK_THROW(QuarkError{fmtlib::format("journal {} is too small: {} bytes", options.path, file_.size())});
        }
        const Scan s = scan(file_.addr(), file_.size(), [](uint64_t, std::span<const uint8_t>) {});
        tail_ = s.end;
        last_seq_ = s.last_seq;
        torn_bytes_ = nonzero_extent(file_.addr() + tail_, file_.size() - tail_);
        prefault_bytes_ = options.prefault_bytes;
        (void)prefault();
    }

    /// Appends `payload`; false if the journal is full or `payload` is empty.
    bool append(std::span<const uint8_t> payload) {
        return append(static_cast<uint32_t>(payload.size()),
                      [&](uint8_t* dst) { std::memcpy(dst, payload.data(), payload.size()); });
    }

    /// Appends a `size`-byte record written in place by `fill(uint8_t* dst)`; false if it doesn't fit
    /// or `size` is 0 (a zero size marks the end of the journal: such a record would hide every later
    /// one from replay).
    template <class Fill>
    bool append(uint32_t size, Fill&& fill) {
        if (size == 0) [[unlikely]] return false;
        const std::size_t total = record_bytes(size);
        if (total > file_.size() - tail_) return false;
        uint8_t* rec = file_.addr() + tail_;
        uint8_t* payload = rec + sizeof(Header);
        fill(payload);
        const uint64_t seq = last_seq_ + 1;
        auto* h = reinterpret_cast<Header*>(rec);
        h->seq = seq;
        h->crc = record_crc(seq, payload, size);
        std::atomic_ref<uint32_t>(h->size).store(size, std::memory_order_release);
        // read by sync()/prefault() on the housekeeping thread
        std::atomic_ref<std::size_t>(tail_).store(tail_ + total, std::memory_order_relaxed);
        last_seq_ = seq;
        return true;
    }

    [[nodiscard]] uint64_t last_seq() const noexcept { return last_seq_; }
    [[nodiscard]] std::size_t bytes_used() const noexcept { return tail_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return file_.size(); }
    /// Extent of non-zero bytes after the last valid record when the file was opened (a torn
    /// write), rounded up to 8 bytes; 0 for a clean file.
    [[nodiscard]] std::size_t torn_bytes() const noexcept { return torn_bytes_; }

    /// Flushes the records to disk, then prefault()s the pages ahead of the tail again (writeback
    /// write-protects the tail page). Safe from another thread. Only `[0, tail)` is written: the
    /// prefaulted pages ahead hold no records, flushing them would only rewrite zeros.
    void sync() const noexcept {
        const std::size_t tail = load_tail();
        if (tail > 0) ::msync(file_.addr(), round_up_to_page(tail), MS_SYNC);
        (void)prefault();
    }

    /// Write-faults the pages from the tail's page to `prefault_bytes` ahead (MADV_POPULATE_WRITE:
    /// the kernel does it without touching the bytes, so it is safe while append() writes there).
    /// Safe from another thread. False if the kernel doesn't support it (Linux < 5.14): appends then
    /// take a page fault per page.
    bool prefault() const noexcept {
        const std::size_t tail = load_tail();
        const std::size_t begin = tail / page_size() * page_size();
        // saturating: a huge prefault_bytes (e.g. SIZE_MAX for "the whole file") must not wrap
        const std::size_t end = prefault_bytes_ >= file_.size() - tail ? file_.size() : tail + prefault_bytes_;
        if (prefault_bytes_ == 0 || begin >= end) return true;
        return ::madvise(file_.addr() + begin, end - begin, MADV_POPULATE_WRITE) == 0;
    }

    /// Calls fn(seq, payload) for each valid record of the journal at `path`, in order. Returns the
    /// number of records. Works on a file another process is appending to.
    template <class Fn>
    static uint64_t replay(const std::string& path, Fn&& fn) {
        MMapFile f;
        f.initialize({.filename = path, .writable = false, .lock = false, .create = false});
        return scan(f.addr(), f.size(), fn).last_seq;
    }

private:
    MMapFile file_;
    alignas(std::atomic_ref<std::size_t>::required_alignment) std::size_t tail_{0};
    std::size_t prefault_bytes_{0};
    uint64_t last_seq_{0};
    std::size_t torn_bytes_{0};

    struct Scan {
        std::size_t end;
        uint64_t last_seq;
    };

    /// Bytes up to and including the last non-zero one (`p` is 8-byte aligned, `n` a multiple of 8).
    static std::size_t nonzero_extent(const uint8_t* p, std::size_t n) noexcept {
        for (std::size_t i = n / 8; i > 0; --i) {
            uint64_t w;
            std::memcpy(&w, p + (i - 1) * 8, 8);
            if (w != 0) return i * 8;
        }
        return 0;
    }

    // the tail as published by append(), for the housekeeping thread (atomic_ref<const T> is C++26)
    std::size_t load_tail() const noexcept {
        return std::atomic_ref<std::size_t>(const_cast<std::size_t&>(tail_)).load(std::memory_order_relaxed);
    }

    static std::size_t page_size() noexcept {
        static const std::size_t size = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        return size;
    }

    std::size_t round_up_to_page(std::size_t n) const noexcept {
        return std::min(file_.size(), (n + page_size() - 1) / page_size() * page_size());
    }

    static constexpr std::size_t record_bytes(std::size_t size) noexcept {
        return (sizeof(Header) + size + kAlign - 1) / kAlign * kAlign;
    }

    static uint32_t record_crc(uint64_t seq, const uint8_t* payload, std::size_t size) noexcept {
        return crc32c(payload, size, crc32c(&seq, sizeof(seq)));
    }

    template <class Fn>
    static Scan scan(const uint8_t* base, std::size_t len, Fn&& fn) {
        std::size_t pos = 0;
        uint64_t seq = 0;
        while (len - pos >= sizeof(Header)) {
            Header h;
            std::memcpy(&h, base + pos, sizeof(h));
            h.size = std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(base + pos)))
                         .load(std::memory_order_acquire);
            if (h.size == 0 || record_bytes(h.size) > len - pos || h.seq != seq + 1) break;
            const uint8_t* payload = base + pos + sizeof(Header);
            if (h.crc != record_crc(h.seq, payload, h.size)) break;
            fn(h.seq, std::span<const uint8_t>(payload, h.size));
            seq = h.seq;
            pos += record_bytes(h.size);
        }
        return {pos, seq};
    }
};

QUARK_END_NAMESPACE
