#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "utils/MathUtils.h"

using namespace Catch;

// Compile-time contract for the C++20 modernization of the pure helpers:
// functions marked constexpr must be usable in constant expressions, and the
// noexcept specifiers must be visible to callers. A regression here fails the
// build instead of showing up at runtime.
static_assert(lrc::lerp(0.0, 1.0, 0.25) == 0.25);
static_assert(lrc::lerp(-2.0, 2.0, 0.5) == 0.0);
static_assert(lrc::smoothstep(0.0, 1.0, 0.0) == 0.0);
static_assert(lrc::smoothstep(0.0, 1.0, 0.5) == 0.5);
static_assert(lrc::smoothstep(0.0, 1.0, 2.0) == 1.0);
static_assert(lrc::hashCombine(0u, 0u) ==
              static_cast<size_t>(0x9e3779b97f4a7c15ull));
static_assert(lrc::hashCombine(1u, 2u) == lrc::hashCombine(1u, 2u));
static_assert(noexcept(lrc::lerp(0.0, 1.0, 0.5)));
static_assert(noexcept(lrc::smoothstep(0.0, 1.0, 0.5)));
static_assert(noexcept(lrc::hashCombine(0u, 0u)));
static_assert(noexcept(lrc::stopsToGain(0.0)));
static_assert(noexcept(lrc::sliderToScale(0.0, 1.0)));

TEST_CASE("MathUtils::clamp clamps into range", "[math]") {
    REQUIRE(lrc::clamp(1.5, 0.0, 1.0) == Approx(1.0));
    REQUIRE(lrc::clamp(-0.5, 0.0, 1.0) == Approx(0.0));
    REQUIRE(lrc::clamp(0.5, 0.0, 1.0) == Approx(0.5));
}

TEST_CASE("MathUtils::stopsToGain follows the EV definition", "[math]") {
    REQUIRE(lrc::stopsToGain(0.0) == Approx(1.0));
    REQUIRE(lrc::stopsToGain(1.0) == Approx(2.0));
    REQUIRE(lrc::stopsToGain(-1.0) == Approx(0.5));
    REQUIRE(lrc::stopsToGain(2.0) == Approx(4.0));
}

TEST_CASE("MathUtils::smoothstep has correct endpoints", "[math]") {
    REQUIRE(lrc::smoothstep(0.0, 1.0, 0.0) == Approx(0.0));
    REQUIRE(lrc::smoothstep(0.0, 1.0, 1.0) == Approx(1.0));
    REQUIRE(lrc::smoothstep(0.0, 1.0, 0.5) == Approx(0.5));
}

TEST_CASE("MathUtils::hashCombine is deterministic", "[math]") {
    const size_t a = lrc::hashCombine(1u, 2u);
    const size_t b = lrc::hashCombine(1u, 2u);
    REQUIRE(a == b);
    REQUIRE(lrc::hashCombine(1u, 2u) != lrc::hashCombine(1u, 3u));
}

TEST_CASE("MathUtils::constexpr helpers agree with runtime evaluation",
          "[math]") {
    // Non-constant inputs force the runtime path; the result must be
    // bit-identical to the compile-time evaluation of the same values.
    volatile double v = 0.25;
    const double runtime = lrc::smoothstep(0.0, 1.0, v);
    constexpr double compileTime = lrc::smoothstep(0.0, 1.0, 0.25);
    REQUIRE(runtime == compileTime);

    volatile double a = -2.0;
    REQUIRE(lrc::lerp(a, 2.0, 0.5) == 0.0);
}

TEST_CASE("MathUtils::sliderToScale maps slider positions to gains", "[math]") {
    REQUIRE(lrc::sliderToScale(0.0, 1.0) == Approx(1.0));
    REQUIRE(lrc::sliderToScale(100.0, 1.0) == Approx(2.0));
    REQUIRE(lrc::sliderToScale(-100.0, 1.0) == Approx(0.5));
}

TEST_CASE("MathUtils::utf8 path round-trip preserves multibyte names", "[math]") {
    // Simulates a UTF-8 filename on a system whose ANSI codepage is not UTF-8.
    const std::string utf8 = "\xe6\xb5\x8b\xe8\xaf\x95\x2e\x61\x72\x77"; // 测试.arw
    const std::filesystem::path p = lrc::utf8ToPath(utf8);
    REQUIRE(lrc::pathToUtf8(p) == utf8);
}
