#include "quark/utils/CircularBuffer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using quark::CircularBuffer;

namespace {

template <class T>
std::vector<T> contents(CircularBuffer<T>& b) {
    std::vector<T> out;
    b.for_each_item([&](T& x) { out.push_back(x); });
    return out;
}

struct Counted {
    static inline int live = 0;
    int v;
    explicit Counted(int x) : v(x) { ++live; }
    Counted(const Counted& o) : v(o.v) { ++live; }
    Counted(Counted&& o) noexcept : v(o.v) { ++live; }
    ~Counted() { --live; }
};

}  // namespace

TEST(CircularBufferTest, FifoOrderAcrossWrap) {
    CircularBuffer<int> b(4);
    for (int i = 0; i < 3; ++i) b.push_back(i);
    b.pop_front();
    b.pop_front();
    for (int i = 3; i < 6; ++i) b.push_back(i);  // wraps: tail passes the end of the storage
    EXPECT_EQ(b.size(), 4u);
    EXPECT_EQ(b.capacity(), 4u);
    EXPECT_EQ(contents(b), (std::vector<int>{2, 3, 4, 5}));
}

TEST(CircularBufferTest, GrowWhileWrappedKeepsOrder) {
    CircularBuffer<int> b(4);
    for (int i = 0; i < 4; ++i) b.push_back(i);
    b.pop_front();
    b.push_back(4);  // full and wrapped
    b.push_back(5);  // grows
    EXPECT_GT(b.capacity(), 4u);
    EXPECT_EQ(contents(b), (std::vector<int>{1, 2, 3, 4, 5}));
}

TEST(CircularBufferTest, PushOfOwnElementWhenFull) {
    // the argument refers into the storage that the growth replaces
    CircularBuffer<std::string> b(2);
    b.push_back(std::string(40, 'a'));
    b.push_back(std::string(40, 'b'));
    b.push_back(b.front());
    EXPECT_EQ(contents(b), (std::vector<std::string>{std::string(40, 'a'), std::string(40, 'b'), std::string(40, 'a')}));
}

TEST(CircularBufferTest, AppendCopiesLvalueRange) {
    CircularBuffer<std::string> b(2);
    const std::vector<std::string> src{std::string(30, 'x'), std::string(30, 'y'), std::string(30, 'z')};
    std::vector<std::string> copy = src;
    b.append(copy.begin(), copy.end());
    EXPECT_EQ(copy, src);  // not moved from
    EXPECT_EQ(contents(b), src);
}

TEST(CircularBufferTest, AppendAcrossWrapExactlyFilling) {
    CircularBuffer<int> b(4);
    b.push_back(0);
    b.push_back(1);
    b.pop_front();
    b.pop_front();  // head and tail at index 2
    const std::vector<int> src{10, 11, 12, 13};
    b.append(src.begin(), src.end());
    EXPECT_EQ(b.capacity(), 4u);
    EXPECT_EQ(contents(b), src);
    b.pop_front();
    b.push_back(14);
    EXPECT_EQ(contents(b), (std::vector<int>{11, 12, 13, 14}));
}

TEST(CircularBufferTest, PopFrontToAcrossWrap) {
    CircularBuffer<int> b(4);
    for (int i = 0; i < 3; ++i) b.push_back(i);
    b.pop_front();
    b.pop_front();
    for (int i = 3; i < 6; ++i) b.push_back(i);  // [2, 3 | 4, 5]
    std::vector<int> out;
    auto [it, n] = b.pop_front_to(4, std::back_inserter(out));
    (void)it;
    EXPECT_EQ(n, 4u);
    EXPECT_EQ(out, (std::vector<int>{2, 3, 4, 5}));
    EXPECT_TRUE(b.empty());
    b.push_back(6);
    EXPECT_EQ(contents(b), (std::vector<int>{6}));
}

TEST(CircularBufferTest, OverAlignedElements) {
    struct alignas(64) Line {
        uint64_t v;
    };
    CircularBuffer<Line> b(3);
    for (uint64_t i = 0; i < 10; ++i) {
        b.push_back(Line{i});  // grows twice
    }
    b.for_each_item([](Line& l) { EXPECT_EQ(reinterpret_cast<uintptr_t>(&l) % 64, 0u); });
}

TEST(CircularBufferTest, ZeroCapacityGrowsOnFirstPush) {
    CircularBuffer<int> b(0);
    b.push_back(7);
    EXPECT_EQ(contents(b), (std::vector<int>{7}));
}

TEST(CircularBufferTest, EveryElementDestroyedOnce) {
    Counted::live = 0;
    {
        CircularBuffer<Counted> b(2);
        for (int i = 0; i < 9; ++i) b.emplace_back(i);  // several growths
        b.pop_front();
        EXPECT_EQ(Counted::live, 8);
        CircularBuffer<Counted> moved(std::move(b));
        EXPECT_EQ(Counted::live, 8);
        EXPECT_EQ(moved.size(), 8u);
    }
    EXPECT_EQ(Counted::live, 0);
}
