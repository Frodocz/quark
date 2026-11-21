#include "quark/utils/LatencyRecorder.h"

#include <gtest/gtest.h>

TEST(TestLatencyRecorder, RecordsPerStageAndResetsOnReport) {
    quark::LatencyRecorder<2> r({"wire", "ack"});
    for (int i = 1; i <= 100; ++i) r.record(0, i * 1000);
    r.record(1, 5'000'000);
    EXPECT_EQ(r.histogram(0).count(), 100u);
    EXPECT_NEAR(static_cast<double>(r.histogram(0).percentile(50)), 50'000, 3'000);
    EXPECT_EQ(r.histogram(1).max(), 5'000'000);
    r.report("test");
    EXPECT_EQ(r.histogram(0).count(), 0u);
    EXPECT_EQ(r.histogram(1).count(), 0u);
}
