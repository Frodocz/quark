#include "quark/utils/MemoryArena.h"
#include "gtest/gtest.h"

#include <unistd.h>

#include <cstdint>
#include <cstring>

using namespace quark;

namespace {
const std::size_t kPage = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
}

class TestMemoryArena : public ::testing::Test {};

TEST_F(TestMemoryArena, BumpAllocationAndAlignment) {
    MemoryArena arena(4096, {.huge_pages = MemoryArena::HugePages::Never});
    EXPECT_EQ(arena.capacity(), kPage);
    EXPECT_EQ(arena.page_kind(), MemoryArena::PageKind::Normal);

    auto* a = static_cast<uint8_t*>(arena.allocate(3, 1));
    auto* b = static_cast<uint8_t*>(arena.allocate(8, 8));
    auto* c = static_cast<uint8_t*>(arena.allocate(100, 64));
    ASSERT_TRUE(a && b && c);
    EXPECT_EQ(b - a, 8);  // a took [0,3), b aligned up to 8
    EXPECT_EQ(reinterpret_cast<uintptr_t>(c) % 64, 0u);
    EXPECT_EQ(arena.used(), static_cast<std::size_t>(c - a) + 100);
    EXPECT_EQ(arena.remaining(), arena.capacity() - arena.used());
    EXPECT_TRUE(arena.contains(a) && arena.contains(c + 99));
    int on_stack = 0;
    EXPECT_FALSE(arena.contains(&on_stack));

    // memory is usable and starts zeroed (fresh anonymous mapping)
    EXPECT_EQ(c[0], 0);
    std::memset(c, 0xAB, 100);
    EXPECT_EQ(c[99], 0xAB);
}

TEST_F(TestMemoryArena, ExhaustionReturnsNull) {
    MemoryArena arena(kPage, {.huge_pages = MemoryArena::HugePages::Never});
    ASSERT_NE(arena.allocate(kPage - 16, 64), nullptr);
    EXPECT_EQ(arena.allocate(32, 1), nullptr);   // doesn't fit
    EXPECT_NE(arena.allocate(16, 1), nullptr);   // exactly fills it
    EXPECT_EQ(arena.remaining(), 0u);
    EXPECT_EQ(arena.allocate(1, 1), nullptr);
}

TEST_F(TestMemoryArena, AlignmentPaddingCanExhaust) {
    MemoryArena arena(kPage, {.huge_pages = MemoryArena::HugePages::Never});
    ASSERT_NE(arena.allocate(kPage - 100, 1), nullptr);
    EXPECT_EQ(arena.allocate(64, 128), nullptr);  // 64 bytes would fit, the aligned start doesn't
}

TEST_F(TestMemoryArena, ZeroCapacityThrows) {
    EXPECT_THROW(MemoryArena(0), QuarkError);
}

TEST_F(TestMemoryArena, HugePagePolicy) {
    // Auto keeps small arenas on 4 KiB pages: a huge page would mostly be wasted
    MemoryArena small(64 * 1024);
    EXPECT_EQ(small.page_kind(), MemoryArena::PageKind::Normal);
    EXPECT_EQ(small.capacity(), 64u * 1024);

    // Auto/Always use huge pages when available; either way the arena spans whole huge pages
    MemoryArena big(3 * 1024 * 1024);
    EXPECT_NE(big.page_kind(), MemoryArena::PageKind::Normal) << "no huge pages of any kind available";
    EXPECT_EQ(big.capacity() % MemoryArena::kHugePageSize, 0u);
    EXPECT_GE(big.capacity(), 3u * 1024 * 1024);

    MemoryArena forced(4096, {.huge_pages = MemoryArena::HugePages::Always});
    EXPECT_EQ(forced.capacity(), MemoryArena::kHugePageSize);

    // the whole arena is usable, including the last byte
    auto* p = static_cast<uint8_t*>(big.allocate(big.capacity(), 64));
    ASSERT_NE(p, nullptr);
    p[big.capacity() - 1] = 1;
}

TEST_F(TestMemoryArena, LockingReported) {
    MemoryArena locked(kPage, {.huge_pages = MemoryArena::HugePages::Never, .lock = true});
    MemoryArena unlocked(kPage, {.huge_pages = MemoryArena::HugePages::Never, .lock = false});
    EXPECT_FALSE(unlocked.locked());
    // mlock may legitimately fail under a small RLIMIT_MEMLOCK; it must then report false
    if (!locked.locked()) GTEST_SKIP() << "mlock not permitted here";
}

#if defined(__SANITIZE_ADDRESS__)
TEST_F(TestMemoryArena, OverrunIntoUnallocatedSpaceReportedUnderAsan) {
    MemoryArena arena(kPage, {.huge_pages = MemoryArena::HugePages::Never});
    auto* p = static_cast<uint8_t*>(arena.allocate(64, 64));
    p[63] = 1;  // fine
    EXPECT_DEATH({ p[64] = 1; }, "use-after-poison");
}
#endif
