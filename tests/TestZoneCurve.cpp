#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "engine/ZoneCurve.h"

using namespace Catch;
using lrc::ToneCurvePoint;
namespace zc = lrc::zonecurve;

TEST_CASE("ZoneCurve: linear curve is an identity point set", "[zonecurve]") {
    const auto c = zc::makeLinear();
    REQUIRE(c.size() == zc::kZoneCount + 2);
    REQUIRE(c.front().x == Approx(0.0f));
    REQUIRE(c.front().y == Approx(0.0f));
    REQUIRE(c.back().x == Approx(1.0f));
    REQUIRE(c.back().y == Approx(1.0f));
    for (size_t i = 0; i < zc::kZoneCount; ++i) {
        REQUIRE(c[i + 1].x == Approx(zc::anchors()[i]));
        REQUIRE(c[i + 1].y == Approx(zc::anchors()[i]));
    }
}

TEST_CASE("ZoneCurve: fromOutputs fills the zones and clamps", "[zonecurve]") {
    const auto c = zc::fromOutputs(0.2f, 0.6f, 0.9f, 1.5f);
    REQUIRE(c[1].y == Approx(0.2f));
    REQUIRE(c[2].y == Approx(0.6f));
    REQUIRE(c[3].y == Approx(0.9f));
    REQUIRE(c[4].y == Approx(1.0f));  // 1.5 is clamped
    REQUIRE(zc::fromOutputs(-0.5f, 0.4f, 0.5f, 0.6f)[1].y ==
            Approx(0.0f));
}

TEST_CASE("ZoneCurve: free-form points are quantized onto the zones",
          "[zonecurve]") {
    // A typical old-editor S-curve, no longer aligned with the zone anchors.
    const std::vector<ToneCurvePoint> oldCurve = {
        {0.00f, 0.00f},
        {0.20f, 0.30f},  // nearest zone: shadows (anchor 0.25)
        {0.50f, 0.70f},  // nearest zone: darks (anchor 0.47)
        {0.90f, 0.80f},  // nearest zone: highlights (anchor 0.82)
        {1.00f, 1.00f},
    };
    const auto c = zc::fromPoints(oldCurve);
    REQUIRE(c[1].y == Approx(0.30f));
    REQUIRE(c[2].y == Approx(0.70f));
    REQUIRE(c[3].y == Approx(zc::anchors()[2]));  // lights untouched
    REQUIRE(c[4].y == Approx(0.80f));
}

TEST_CASE("ZoneCurve: fromOutputs round-trips through fromPoints",
          "[zonecurve]") {
    const auto c = zc::fromOutputs(0.10f, 0.40f, 0.70f, 0.95f);
    REQUIRE(zc::fromPoints(c) == c);
}

TEST_CASE("ZoneCurve: interpolate reproduces linear data exactly",
          "[zonecurve]") {
    const auto linear = zc::makeLinear();
    for (int s = 0; s <= 200; ++s) {
        const float x = static_cast<float>(s) / 200.0f;
        REQUIRE(zc::interpolate(linear, x) == Approx(x).margin(1e-4f));
    }
}

TEST_CASE("ZoneCurve: interpolate passes through the control points",
          "[zonecurve]") {
    const auto c = zc::fromOutputs(0.15f, 0.55f, 0.80f, 0.95f);
    REQUIRE(zc::interpolate(c, 0.0f) == Approx(0.0f));
    REQUIRE(zc::interpolate(c, 1.0f) == Approx(1.0f));
    const auto ax = zc::anchors();
    for (size_t i = 0; i < zc::kZoneCount; ++i)
        REQUIRE(zc::interpolate(c, ax[i]) == Approx(c[i + 1].y));
}

TEST_CASE("ZoneCurve: interpolate stays monotone and never overshoots",
          "[zonecurve]") {
    // A pronounced S shape; unlike Catmull-Rom the interpolant must not ring.
    const auto c = zc::fromOutputs(0.05f, 0.35f, 0.70f, 0.95f);
    float prev = -1.0f;
    constexpr int kSamples = 200;
    for (int s = 0; s <= kSamples; ++s) {
        const float x = static_cast<float>(s) / kSamples;
        const float y = zc::interpolate(c, x);
        REQUIRE(y >= 0.0f);
        REQUIRE(y <= 1.0f);
        REQUIRE(y >= prev);  // monotone non-decreasing
        prev = y;
    }
}

TEST_CASE("ZoneCurve: interpolate degrades gracefully for two points",
          "[zonecurve]") {
    const std::vector<ToneCurvePoint> two = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    REQUIRE(zc::interpolate(two, 0.25f) == Approx(0.25f));
    REQUIRE(zc::interpolate(two, 0.75f) == Approx(0.75f));
    REQUIRE(zc::interpolate({}, 0.5f) == Approx(0.5f));
}
