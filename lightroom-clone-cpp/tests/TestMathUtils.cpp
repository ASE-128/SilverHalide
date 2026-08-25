#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "utils/MathUtils.h"

using namespace Catch;

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

TEST_CASE("MathUtils::utf8 path round-trip preserves multibyte names", "[math]") {
    // Simulates a UTF-8 filename on a system whose ANSI codepage is not UTF-8.
    const std::string utf8 = "\xe6\xb5\x8b\xe8\xaf\x95\x2e\x61\x72\x77"; // 测试.arw
    const std::filesystem::path p = lrc::utf8ToPath(utf8);
    REQUIRE(lrc::pathToUtf8(p) == utf8);
}
