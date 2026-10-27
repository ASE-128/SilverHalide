// Performance diagnostics: the 30-sample timing window behind the status-bar
// chip and the 视图 → 性能诊断… dialog.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <vector>

#include "utils/PerfStats.h"

using namespace Catch;
using namespace lrc;

TEST_CASE("PerfStats: empty window reports zero everywhere", "[perf][stats]") {
    PerfStats stats;
    REQUIRE(stats.windowSize() == 30);
    REQUIRE(stats.sampleCount() == 0);
    REQUIRE(stats.medianMs() == Approx(0.0));
    REQUIRE(stats.minMs() == Approx(0.0));
    REQUIRE(stats.maxMs() == Approx(0.0));
}

TEST_CASE("PerfStats: single sample is min, median and max", "[perf][stats]") {
    PerfStats stats;
    stats.add(12.5);
    REQUIRE(stats.sampleCount() == 1);
    REQUIRE(stats.medianMs() == Approx(12.5));
    REQUIRE(stats.minMs() == Approx(12.5));
    REQUIRE(stats.maxMs() == Approx(12.5));
}

TEST_CASE("PerfStats: median is order independent", "[perf][stats]") {
    PerfStats stats;
    // Odd count: the middle value.
    for (const double v : {9.0, 1.0, 7.0, 3.0, 5.0})
        stats.add(v);
    REQUIRE(stats.sampleCount() == 5);
    REQUIRE(stats.medianMs() == Approx(5.0));
    REQUIRE(stats.minMs() == Approx(1.0));
    REQUIRE(stats.maxMs() == Approx(9.0));

    stats.clear();
    // Even count: mean of the two middle values.
    for (const double v : {4.0, 1.0, 3.0, 2.0})
        stats.add(v);
    REQUIRE(stats.medianMs() == Approx(2.5));
}

TEST_CASE("PerfStats: window keeps the last 30 samples", "[perf][stats]") {
    PerfStats stats;
    // 1..29: partial window, median = middle sample.
    for (int i = 1; i <= 29; ++i)
        stats.add(static_cast<double>(i));
    REQUIRE(stats.sampleCount() == 29);
    REQUIRE(stats.medianMs() == Approx(15.0));
    REQUIRE(stats.minMs() == Approx(1.0));
    REQUIRE(stats.maxMs() == Approx(29.0));

    // The 30th sample fills the window.
    stats.add(30.0);
    REQUIRE(stats.sampleCount() == 30);
    REQUIRE(stats.medianMs() == Approx(15.5));

    // Samples 31.. evict the oldest; window stays at 30 and tracks the tail.
    for (int i = 31; i <= 40; ++i)
        stats.add(static_cast<double>(i));
    REQUIRE(stats.sampleCount() == 30);
    // Retained set is 11..40: median of 25 and 26 (0-based 14/15) = 25.5.
    REQUIRE(stats.medianMs() == Approx(25.5));
    REQUIRE(stats.minMs() == Approx(11.0));
    REQUIRE(stats.maxMs() == Approx(40.0));
}

TEST_CASE("PerfStats: constant samples and invalids", "[perf][stats]") {
    PerfStats stats;
    for (int i = 0; i < 40; ++i)
        stats.add(6.0);
    REQUIRE(stats.sampleCount() == 30);
    REQUIRE(stats.medianMs() == Approx(6.0));
    REQUIRE(stats.minMs() == Approx(6.0));
    REQUIRE(stats.maxMs() == Approx(6.0));

    // Negative / non-finite samples are not durations and are ignored.
    const std::size_t before = stats.sampleCount();
    stats.add(-1.0);
    stats.add(std::numeric_limits<double>::quiet_NaN());
    REQUIRE(stats.sampleCount() == before);
}

TEST_CASE("PerfStats: clear resets the window", "[perf][stats]") {
    PerfStats stats;
    stats.add(1.0);
    stats.add(2.0);
    stats.clear();
    REQUIRE(stats.sampleCount() == 0);
    REQUIRE(stats.medianMs() == Approx(0.0));
    REQUIRE(stats.minMs() == Approx(0.0));
    REQUIRE(stats.maxMs() == Approx(0.0));

    // The window is reusable after clear.
    stats.add(4.0);
    REQUIRE(stats.medianMs() == Approx(4.0));
}
