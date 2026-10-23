#include "quark/utils/InplaceFunction.h"
#include "gtest/gtest.h"

#include <array>
#include <functional>
#include <memory>

using namespace quark;

class TestInplaceFunction : public ::testing::Test {};

TEST_F(TestInplaceFunction, StoresCapturesInline) {
    std::array<int, 12> big{};  // 48 bytes of captured state
    big[11] = 7;
    InplaceFunction<int(int), 64> f = [big](int x) { return big[11] + x; };
    EXPECT_EQ(f(1), 8);
    static_assert(sizeof(f) <= 64 + 2 * sizeof(void*));  // storage + vtable pointer, no heap

    InplaceFunction<int(int), 64> g = f;  // copyable
    EXPECT_EQ(g(2), 9);
    InplaceFunction<int(int), 64> h = std::move(f);
    EXPECT_EQ(h(3), 10);
}

TEST_F(TestInplaceFunction, EmptyAndReset) {
    InplaceFunction<void()> f;
    EXPECT_FALSE(f);
    int calls = 0;
    f = [&calls] { ++calls; };
    EXPECT_TRUE(f);
    f();
    EXPECT_EQ(calls, 1);
    f = nullptr;
    EXPECT_FALSE(f);
#if !defined(QUARK_NO_EXCEPTIONS)
    EXPECT_THROW(f(), std::bad_function_call);
#endif
}

TEST_F(TestInplaceFunction, DestroysCapturedState) {
    auto token = std::make_shared<int>(1);
    {
        InplaceFunction<int()> f = [token] { return *token; };
        EXPECT_EQ(token.use_count(), 2);
        f = nullptr;
        EXPECT_EQ(token.use_count(), 1);
    }
    EXPECT_EQ(token.use_count(), 1);
}
