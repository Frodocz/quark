#include <gtest/gtest.h>

#include "quark/Attributes.h"

static_assert(quark::kVersion == QUARK_VERSION_MAJOR * 10000 + QUARK_VERSION_MINOR * 100 + QUARK_VERSION_PATCH);

TEST(Version, Namespaced) {
    EXPECT_EQ(quark::kVersion / 10000, QUARK_VERSION_MAJOR);
    EXPECT_EQ(quark::kVersion / 100 % 100, QUARK_VERSION_MINOR);
    EXPECT_EQ(quark::kVersion % 100, QUARK_VERSION_PATCH);
}
