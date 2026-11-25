#include "quark/utils/Journal.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <sys/resource.h>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace quark;

namespace {

class TestJournal : public ::testing::Test {
protected:
    std::string path = (std::filesystem::temp_directory_path() / ("quark_journal_" + std::to_string(::getpid()))).string();

    void SetUp() override { std::filesystem::remove(path); }
    void TearDown() override { std::filesystem::remove(path); }

    Journal open(std::size_t capacity = 1 << 16) { return Journal({.path = path, .capacity = capacity, .lock = false}); }

    std::vector<std::pair<uint64_t, std::string>> records() const {
        std::vector<std::pair<uint64_t, std::string>> out;
        Journal::replay(path, [&](uint64_t seq, std::span<const uint8_t> p) {
            out.emplace_back(seq, std::string(reinterpret_cast<const char*>(p.data()), p.size()));
        });
        return out;
    }
};

std::span<const uint8_t> bytes(std::string_view s) { return {reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }

}  // namespace

TEST(TestCrc32c, KnownValue) {
    EXPECT_EQ(crc32c("123456789", 9), 0xE3069283u);
    EXPECT_EQ(crc32c("", 0), 0u);
}

TEST(TestCrc32c, EveryImplementationAgrees) {
    std::vector<uint8_t> data(1031);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 131 + 7);
    for (std::size_t n : {0u, 1u, 7u, 8u, 9u, 64u, 1031u}) {
        const uint32_t sw = ~detail::crc32c_software(data.data(), n, ~0u);
        EXPECT_EQ(crc32c(data.data(), n), sw) << n;
#if defined(QUARK_CRC32C_X86)
        if (detail::has_sse42()) {
            EXPECT_EQ(~detail::crc32c_sse42(data.data(), n, ~0u), sw) << n;
        }
#endif
    }
    // continuing from a previous CRC equals one pass over the whole buffer
    EXPECT_EQ(crc32c(data.data() + 100, 931, crc32c(data.data(), 100)), crc32c(data.data(), 1031));
}

TEST_F(TestJournal, AppendsAndReplaysInOrder) {
    {
        Journal j = open();
        EXPECT_TRUE(j.append(bytes("new order")));
        EXPECT_TRUE(j.append(5, [](uint8_t* dst) { std::memcpy(dst, "ack:1", 5); }));  // written in place
        EXPECT_EQ(j.last_seq(), 2u);
        EXPECT_EQ(j.bytes_used() % Journal::kAlign, 0u);
    }
    using R = std::vector<std::pair<uint64_t, std::string>>;
    EXPECT_EQ(records(), (R{{1, "new order"}, {2, "ack:1"}}));
}

TEST_F(TestJournal, ReopenResumesAfterTheLastRecord) {
    open().append(bytes("a"));
    {
        Journal j = open();
        EXPECT_EQ(j.last_seq(), 1u);
        EXPECT_EQ(j.torn_bytes(), 0u);
        j.append(bytes("b"));
    }
    using R = std::vector<std::pair<uint64_t, std::string>>;
    EXPECT_EQ(records(), (R{{1, "a"}, {2, "b"}}));
}

TEST_F(TestJournal, TornRecordIsDroppedAndOverwritten) {
    std::size_t third_at;
    {
        Journal j = open();
        j.append(bytes("one"));
        j.append(bytes("two"));
        third_at = j.bytes_used();
        j.append(bytes("three, cut short by a crash"));
    }
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(static_cast<std::streamoff>(third_at + sizeof(Journal::Header) + 10));
        f.put('X');  // corrupts the payload: CRC mismatch
    }
    Journal j = open();
    EXPECT_EQ(j.last_seq(), 2u);
    EXPECT_GT(j.torn_bytes(), 0u);
    j.append(bytes("3"));
    using R = std::vector<std::pair<uint64_t, std::string>>;
    EXPECT_EQ(records(), (R{{1, "one"}, {2, "two"}, {3, "3"}}));
}

// A record of size 0 is the end-of-journal marker: an empty append must be refused, or it would
// hide every later record from replay (and the next reopen would overwrite them).
TEST_F(TestJournal, EmptyAppendIsRefused) {
    {
        Journal j = open();
        EXPECT_TRUE(j.append(bytes("one")));
        EXPECT_FALSE(j.append(bytes("")));
        EXPECT_FALSE(j.append(0, [](uint8_t*) {}));
        EXPECT_EQ(j.last_seq(), 1u);
        EXPECT_TRUE(j.append(bytes("two")));
    }
    using R = std::vector<std::pair<uint64_t, std::string>>;
    EXPECT_EQ(records(), (R{{1, "one"}, {2, "two"}}));
    Journal again = open();
    EXPECT_EQ(again.last_seq(), 2u);
}

namespace {
long thread_minor_faults() {
    rusage u{};
    ::getrusage(RUSAGE_THREAD, &u);
    return u.ru_minflt;
}
}  // namespace

// A file-backed shared mapping is write-protected until each page's first write, also when mlock'ed,
// and again after it is written back: without prefaulting, every 4 KiB of appends took a page fault
// (~3 us, sometimes ms) on the appending thread.
TEST_F(TestJournal, AppendsWithinThePrefaultWindowDoNotFault) {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || QUARK_HAS_FEATURE(address_sanitizer) || \
    QUARK_HAS_FEATURE(thread_sanitizer)
    GTEST_SKIP() << "sanitizers fault their shadow memory in on first access: no fault count to check";
#endif
    Journal j({.path = path, .capacity = 8u << 20, .lock = true, .prefault_bytes = 2u << 20});
    if (!j.prefault()) GTEST_SKIP() << "kernel without MADV_POPULATE_WRITE (Linux < 5.14)";
    const std::string rec(100, 'x');
    long faults = thread_minor_faults();
    for (int i = 0; i < 10'000; ++i) ASSERT_TRUE(j.append(bytes(rec)));  // ~1.1 MiB, ~280 pages
    EXPECT_LE(thread_minor_faults() - faults, 2);

    // sync() writes the records back, which write-protects their pages again, then prefaults from the tail on
    j.sync();
    faults = thread_minor_faults();
    for (int i = 0; i < 10'000; ++i) ASSERT_TRUE(j.append(bytes(rec)));
    EXPECT_LE(thread_minor_faults() - faults, 2);
    EXPECT_EQ(records().size(), 20'000u);
}

// The tradeserver appends on its loop thread while a housekeeping thread calls sync()
TEST_F(TestJournal, SyncRunsConcurrentlyWithAppends) {
    Journal j({.path = path, .capacity = 4u << 20, .lock = false, .prefault_bytes = 256u << 10});
    std::atomic<bool> done{false};
    std::thread syncer([&] {
        while (!done.load(std::memory_order_acquire)) j.sync();
    });
    const std::string rec(40, 'y');
    for (int i = 0; i < 50'000; ++i) ASSERT_TRUE(j.append(bytes(rec)));
    done.store(true, std::memory_order_release);
    syncer.join();
    EXPECT_EQ(records().size(), 50'000u);
}

TEST_F(TestJournal, FullJournalRefusesAppends) {
    Journal j = open(64);  // room for two 16 + 16 byte records
    EXPECT_TRUE(j.append(bytes("0123456789abcdef")));
    EXPECT_TRUE(j.append(bytes("0123456789abcdef")));
    EXPECT_FALSE(j.append(bytes("x")));
    EXPECT_EQ(j.last_seq(), 2u);
}
