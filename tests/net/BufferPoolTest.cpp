#include "quark/net/BufferPool.h"
#include "gtest/gtest.h"

#include <cstring>
#include <set>
#include <vector>

using namespace quark;
using namespace quark::net;

class TestBufferPool : public ::testing::Test {};

TEST_F(TestBufferPool, SizesAreCacheLineRounded) {
    BufferPool pool(4, 100);
    EXPECT_EQ(pool.buffer_size(), 128u);
    EXPECT_EQ(pool.capacity(), 4u);
    EXPECT_EQ(pool.available(), 4u);

    IOBuffer b = pool.acquire();
    ASSERT_TRUE(b);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(b.data()) % BufferPool::kAlignment, 0u);
    EXPECT_EQ(b.capacity(), 128u);
    EXPECT_EQ(b.size(), 0u);
    EXPECT_EQ(b.tailroom(), 128u);
    pool.release(b);
}

TEST_F(TestBufferPool, InvalidArgumentsThrow) {
    EXPECT_THROW(BufferPool(0, 64), QuarkError);
    EXPECT_THROW(BufferPool(4, 0), QuarkError);
}

TEST_F(TestBufferPool, ExhaustionAndReuse) {
    BufferPool pool(3, 64);
    std::set<uint8_t*> seen;
    std::vector<IOBuffer> held;
    for (int i = 0; i < 3; ++i) {
        held.push_back(pool.acquire());
        ASSERT_TRUE(held.back());
        seen.insert(held.back().data());
    }
    EXPECT_EQ(seen.size(), 3u);  // distinct buffers
    EXPECT_EQ(pool.available(), 0u);
    EXPECT_FALSE(pool.acquire());  // exhausted: invalid handle, no growth

    for (auto& b : held) {
        pool.release(b);
        EXPECT_FALSE(b);  // handle cleared
    }
    EXPECT_EQ(pool.available(), 3u);
    IOBuffer empty;
    pool.release(empty);  // no-op
    EXPECT_EQ(pool.available(), 3u);
}

TEST_F(TestBufferPool, AddressOrderThenLifoReuse) {
    BufferPool pool(4, 64);
    IOBuffer a = pool.acquire();
    IOBuffer b = pool.acquire();
    EXPECT_EQ(b.data() - a.data(), 64);  // fresh buffers come out in address order
    uint8_t* const a_addr = a.data();
    pool.release(a);
    IOBuffer c = pool.acquire();
    EXPECT_EQ(c.data(), a_addr);  // most recently released (cache-hot) buffer first
    pool.release(b);
    pool.release(c);
}

TEST_F(TestBufferPool, HugePagesForLargePools) {
    // 1024 x 4 KiB = 4 MiB: Auto uses huge pages (reserved or transparent) when the kernel has any
    BufferPool big(1024, 4096);
    EXPECT_NE(big.page_kind(), MemoryArena::PageKind::Normal);
    BufferPool small(8, 4096);
    EXPECT_EQ(small.page_kind(), MemoryArena::PageKind::Normal);
    BufferPool never(1024, 4096, {.huge_pages = MemoryArena::HugePages::Never});
    EXPECT_EQ(never.page_kind(), MemoryArena::PageKind::Normal);

    IOBuffer b = big.acquire();
    ASSERT_TRUE(b);
    std::memset(b.append(4096).data(), 0xAB, 4096);
    big.release(b);
}

TEST_F(TestBufferPool, SharedArena) {
    // one arena per event loop, shared by a receive pool and a send pool
    MemoryArena arena(64 * 1024, {.huge_pages = MemoryArena::HugePages::Never});
    BufferPool recv(arena, 4, 8192);
    BufferPool send(arena, 16, 256);
    EXPECT_EQ(arena.used(), 4u * 8192 + 16u * 256);

    IOBuffer r = recv.acquire();
    IOBuffer s = send.acquire();
    EXPECT_TRUE(arena.contains(r.data()));
    EXPECT_TRUE(arena.contains(s.data()));
    EXPECT_EQ(r.capacity(), 8192u);
    EXPECT_EQ(s.capacity(), 256u);
    recv.release(r);
    send.release(s);

    EXPECT_THROW(BufferPool(arena, 64, 8192), QuarkError);  // doesn't fit in what's left
}

TEST_F(TestBufferPool, PrependAppendConsume) {
    BufferPool pool(1, 64);
    IOBuffer b = pool.acquire(/*reserve_front=*/14);  // max WebSocket client header
    EXPECT_EQ(b.headroom(), 14u);
    EXPECT_EQ(b.tailroom(), 50u);

    std::memcpy(b.append(5).data(), "hello", 5);
    EXPECT_EQ(b.size(), 5u);
    EXPECT_EQ(std::memcmp(b.data(), "hello", 5), 0);

    auto hdr = b.prepend(2);
    hdr[0] = 0x81;
    hdr[1] = 0x05;
    EXPECT_EQ(b.headroom(), 12u);
    EXPECT_EQ(b.size(), 7u);
    EXPECT_EQ(b.data()[0], 0x81);
    EXPECT_EQ(std::memcmp(b.data() + 2, "hello", 5), 0);  // payload never moved

    b.consume(2);
    EXPECT_EQ(b.size(), 5u);
    EXPECT_EQ(std::memcmp(b.data(), "hello", 5), 0);

    b.compact();
    EXPECT_EQ(b.headroom(), 0u);
    EXPECT_EQ(b.tailroom(), 59u);
    EXPECT_EQ(std::memcmp(b.data(), "hello", 5), 0);
    EXPECT_EQ(b.spare().size(), 59u);

    b.reset(8);
    EXPECT_TRUE(b.empty());
    EXPECT_EQ(b.headroom(), 8u);
    pool.release(b);
}

#ifndef NDEBUG
TEST_F(TestBufferPool, DoubleReleaseAndForeignBufferDetectedInDebug) {
    BufferPool pool(2, 64);
    BufferPool other(2, 64);
    IOBuffer b = pool.acquire();
    IOBuffer copy = b;  // handles are copyable views: releasing both is a double release
    pool.release(b);
    EXPECT_DEATH(pool.release(copy), "double release");
    IOBuffer foreign = other.acquire();
    EXPECT_DEATH(pool.release(foreign), "not from this pool");
    other.release(foreign);
}
#endif

#if defined(__SANITIZE_ADDRESS__)
TEST_F(TestBufferPool, UseAfterReleaseReportedUnderAsan) {
    BufferPool pool(2, 64);
    IOBuffer b = pool.acquire();
    uint8_t* p = b.data();
    pool.release(b);
    EXPECT_DEATH({ volatile uint8_t x = *p; (void)x; }, "use-after-poison");
}
#endif
