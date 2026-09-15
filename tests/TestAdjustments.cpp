#include <catch2/catch_test_macros.hpp>

#include "engine/Adjustments.h"

TEST_CASE("Adjustments: defaults are neutral", "[adjustments]") {
    const lrc::Adjustments a;
    REQUIRE(a.exposure == 0.0);
    REQUIRE(a.contrast == 0.0);
    REQUIRE(a.highlights == 0.0);
    REQUIRE(a.shadows == 0.0);
    REQUIRE(a.temperature == 0.0);
    REQUIRE(a.tint == 0.0);
    REQUIRE(a.saturation == 0.0);
    REQUIRE(a.luminance == 0.0);
    REQUIRE(a.colorNoiseReduction == 0.0);
    REQUIRE(a.curve.empty());
    for (const auto& ch : a.hsl) {
        REQUIRE(ch.hue == 0.0);
        REQUIRE(ch.saturation == 0.0);
        REQUIRE(ch.luminance == 0.0);
    }
    REQUIRE(a.splitTone.highlightHue == 0.0);
    REQUIRE(a.splitTone.highlightSaturation == 0.0);
    REQUIRE(a.splitTone.shadowHue == 0.0);
    REQUIRE(a.splitTone.shadowSaturation == 0.0);
    REQUIRE(a.splitTone.balance == 0.0);
}

TEST_CASE("Adjustments: whites/blacks/grain defaults", "[adjustments]") {
    const lrc::Adjustments a;
    REQUIRE(a.whites == 0.0);
    REQUIRE(a.blacks == 0.0);
    REQUIRE(a.grain == 0.0);
    REQUIRE(a.grainSize == 25.0);
    REQUIRE(a.grainRoughness == 50.0);
}

TEST_CASE("Adjustments: noise reduction defaults", "[adjustments]") {
    const lrc::Adjustments a;
    REQUIRE(a.luminance == 0.0);
    REQUIRE(a.luminanceDetail == 50.0);
    REQUIRE(a.luminanceContrast == 0.0);
    REQUIRE(a.colorNoiseReduction == 0.0);
    REQUIRE(a.colorNoiseReductionSmoothness == 50.0);
}

TEST_CASE("Adjustments: whites/blacks/grain participate in equality and hash",
          "[adjustments]") {
    lrc::Adjustments a;
    lrc::Adjustments b;
    REQUIRE(a == b);
    REQUIRE(a.hash() == b.hash());
    a.whites = 40.0;
    REQUIRE(a != b);
    REQUIRE(a.hash() != b.hash());
    a.whites = 0.0;
    b.blacks = -30.0;
    REQUIRE(a != b);
    REQUIRE(a.hash() != b.hash());
    b.blacks = 0.0;
    a.grain = 12.0;
    REQUIRE(a != b);
    REQUIRE(a.hash() != b.hash());
    a.grain = 0.0;
    b.grainSize = 80.0;
    REQUIRE(a != b);
    REQUIRE(a.hash() != b.hash());
}

TEST_CASE("Adjustments: noise reduction fields participate in equality and hash",
          "[adjustments]") {
    lrc::Adjustments a;
    lrc::Adjustments b;
    REQUIRE(a == b);
    REQUIRE(a.hash() == b.hash());
    a.luminance = 45.0;
    REQUIRE(a != b);
    REQUIRE(a.hash() != b.hash());
    a.luminance = 0.0;
    b.luminanceDetail = 65.0;
    REQUIRE(a != b);
    REQUIRE(a.hash() != b.hash());
    b.luminanceDetail = 50.0;
    a.luminanceContrast = 20.0;
    REQUIRE(a != b);
    REQUIRE(a.hash() != b.hash());
    a.luminanceContrast = 0.0;
    b.colorNoiseReduction = 35.0;
    REQUIRE(a != b);
    REQUIRE(a.hash() != b.hash());
    b.colorNoiseReduction = 0.0;
    a.colorNoiseReductionSmoothness = 70.0;
    REQUIRE(a != b);
    REQUIRE(a.hash() != b.hash());
}

TEST_CASE("Adjustments: reset restores defaults", "[adjustments]") {
    lrc::Adjustments a;
    a.exposure = 2.5;
    a.saturation = -40;
    a.curve.push_back({0.2f, 0.3f});
    a.hsl[3].saturation = 50.0;
    a.splitTone.highlightHue = 30.0;
    a.reset();
    REQUIRE(a == lrc::Adjustments::defaults());
}

TEST_CASE("Adjustments: equality is exact", "[adjustments]") {
    lrc::Adjustments a;
    lrc::Adjustments b;
    REQUIRE(a == b);
    a.temperature = 12.0;
    REQUIRE(a != b);
    a.temperature = 0.0;
    REQUIRE(a == b);
    a.hsl[2].luminance = -20.0;
    REQUIRE(a != b);
    a.hsl[2].luminance = 0.0;
    REQUIRE(a == b);
    a.splitTone.balance = 50.0;
    REQUIRE(a != b);
    a.splitTone.balance = 0.0;
    REQUIRE(a == b);
}

TEST_CASE("Adjustments: hash matches for equal, differs for different",
          "[adjustments]") {
    lrc::Adjustments a;
    lrc::Adjustments b;
    REQUIRE(a.hash() == b.hash());
    b.contrast = 33.0;
    REQUIRE(a.hash() != b.hash());
    b.contrast = 0.0;
    REQUIRE(a.hash() == b.hash());
}

TEST_CASE("Adjustments: curve participates in hash", "[adjustments]") {
    lrc::Adjustments a;
    lrc::Adjustments b;
    a.curve.push_back({0.25f, 0.5f});
    b.curve.push_back({0.25f, 0.51f});
    REQUIRE(a.hash() != b.hash());
}

TEST_CASE("Adjustments: HSL fields participate in equality and hash",
          "[adjustments][hsl]") {
    lrc::Adjustments a;
    lrc::Adjustments b;
    REQUIRE(a.hash() == b.hash());
    a.hsl[1].hue = 15.0;
    REQUIRE(a != b);
    REQUIRE(a.hash() != b.hash());
    a.hsl[1].hue = 0.0;
    REQUIRE(a == b);
    REQUIRE(a.hash() == b.hash());
}

TEST_CASE("Adjustments: split tone fields participate in equality and hash",
          "[adjustments][split]") {
    lrc::Adjustments a;
    lrc::Adjustments b;
    REQUIRE(a.hash() == b.hash());
    a.splitTone.highlightSaturation = 30.0;
    REQUIRE(a != b);
    REQUIRE(a.hash() != b.hash());
    a.splitTone.highlightSaturation = 0.0;
    REQUIRE(a == b);
    REQUIRE(a.hash() == b.hash());
}
