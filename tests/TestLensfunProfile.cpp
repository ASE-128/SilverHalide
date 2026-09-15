#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <vector>

#include "engine/Adjustments.h"
#include "engine/LensfunProfile.h"
#include "utils/ProfileIO.h"

using namespace lrc;
using Catch::Approx;

TEST_CASE("Lensfun profile adapter reports disabled builds safely", "[lensfun]") {
    const auto profiles = lrc::LensfunProfile::find("", "", "", "");
#if LRC_ENABLE_LENSFUN
    REQUIRE(lrc::LensfunProfile::available());
#else
    REQUIRE_FALSE(lrc::LensfunProfile::available());
    REQUIRE(profiles.empty());
#endif
}

TEST_CASE("Lensfun fields survive a ProfileIO JSON round-trip", "[lensfun][profileio]") {
    Adjustments a;
    a.lensfunEnabled = true;
    a.lensfunCameraMaker = "Canon";
    a.lensfunCameraModel = "EOS 5D Mark III";
    a.lensfunLensMaker = "Canon";
    a.lensfunLensModel = "EF 24-70mm f/2.8L II USM";
    a.lensfunFocalLengthMm = 50.0f;
    a.lensfunAperture = 4.0f;
    a.lensfunCropFactor = 1.0f;

    const json::Value json = ProfileIO::toJson(a);
    const Adjustments r = ProfileIO::fromJson(json);

    REQUIRE(r.lensfunEnabled == a.lensfunEnabled);
    REQUIRE(r.lensfunCameraMaker == a.lensfunCameraMaker);
    REQUIRE(r.lensfunCameraModel == a.lensfunCameraModel);
    REQUIRE(r.lensfunLensMaker == a.lensfunLensMaker);
    REQUIRE(r.lensfunLensModel == a.lensfunLensModel);
    REQUIRE(r.lensfunFocalLengthMm == Approx(a.lensfunFocalLengthMm).margin(1e-6f));
    REQUIRE(r.lensfunAperture == Approx(a.lensfunAperture).margin(1e-6f));
    REQUIRE(r.lensfunCropFactor == Approx(a.lensfunCropFactor).margin(1e-6f));
    // The Lensfun fields participate in equality, so the whole stack matches.
    REQUIRE(r == a);
}

TEST_CASE("Lensfun missing-key load keeps defaults and does not crash",
          "[lensfun][profileio]") {
    // A payload without any Lensfun keys must leave the defaults untouched
    // (regression guard for the asString() no-default-argument fix).
    const auto parsed = json::Value::parse(R"({"exposure":0.5})");
    REQUIRE(parsed.has_value());
    const Adjustments r = ProfileIO::fromJson(*parsed);
    REQUIRE_FALSE(r.lensfunEnabled);
    REQUIRE(r.lensfunCameraMaker.empty());
    REQUIRE(r.lensfunCameraModel.empty());
    REQUIRE(r.lensfunLensMaker.empty());
    REQUIRE(r.lensfunLensModel.empty());
    REQUIRE(r.lensfunFocalLengthMm == Approx(0.0f).margin(1e-6f));
    REQUIRE(r.lensfunAperture == Approx(0.0f).margin(1e-6f));
    REQUIRE(r.lensfunCropFactor == Approx(1.0f).margin(1e-6f));
}

TEST_CASE("Adjustments equality and hash react to Lensfun toggle",
          "[lensfun][adjustments]") {
    Adjustments base;
    base.lensfunCameraMaker = "Canon";
    base.lensfunLensModel = "EF 50mm f/1.8 STM";
    base.lensfunFocalLengthMm = 50.0f;
    base.lensfunAperture = 1.8f;
    base.lensfunCropFactor = 1.0f;

    Adjustments disabled = base;
    disabled.lensfunEnabled = false;
    Adjustments enabled = base;
    enabled.lensfunEnabled = true;

    // Toggling the flag breaks equality and changes the hash.
    REQUIRE(disabled != enabled);
    REQUIRE(disabled.hash() != enabled.hash());

    // Identical Lensfun state compares equal and hashes identically.
    Adjustments enabled2 = enabled;
    REQUIRE(enabled2 == enabled);
    REQUIRE(enabled2.hash() == enabled.hash());

    // A focal-length change also breaks equality/hash (capture params matter).
    Adjustments diffFocal = enabled;
    diffFocal.lensfunFocalLengthMm = 85.0f;
    REQUIRE(diffFocal != enabled);
    REQUIRE(diffFocal.hash() != enabled.hash());
}

// Builds a small synthetic F32 RGB image with a distinguishable non-uniform
// pattern so a real geometry correction would move pixels.
static std::vector<float> makePatternImage(int w, int h) {
    std::vector<float> img(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 3;
            img[i] = static_cast<float>(x) / static_cast<float>(w);
            img[i + 1] = static_cast<float>(y) / static_cast<float>(h);
            img[i + 2] = 0.5f;
        }
    }
    return img;
}

TEST_CASE("Lensfun geometry leaves pixels unchanged when disabled",
          "[lensfun]") {
    constexpr int w = 32;
    constexpr int h = 32;
    auto pixels = makePatternImage(w, h);
    const auto original = pixels;

    Adjustments a;
    a.lensfunEnabled = false;  // explicitly off
    a.lensfunCameraMaker = "Canon";
    a.lensfunCameraModel = "EOS 5D Mark III";
    a.lensfunLensMaker = "Canon";
    a.lensfunLensModel = "EF 24-70mm f/2.8L II USM";
    a.lensfunFocalLengthMm = 50.0f;
    a.lensfunAperture = 4.0f;

    const bool ok = LensfunProfile::applyGeometry(a, pixels.data(), w, h);
    REQUIRE_FALSE(ok);
    REQUIRE(pixels == original);
}

TEST_CASE("Lensfun geometry leaves pixels unchanged when no profile matches",
          "[lensfun]") {
    constexpr int w = 32;
    constexpr int h = 32;
    auto pixels = makePatternImage(w, h);
    const auto original = pixels;

    Adjustments a;
    a.lensfunEnabled = true;
    // Nonsensical identity: the database lookup will fail, so applyGeometry
    // must return false and not touch the buffer.
    a.lensfunCameraMaker = "NoSuchMaker";
    a.lensfunCameraModel = "NoSuchCamera";
    a.lensfunLensMaker = "NoSuchLensMaker";
    a.lensfunLensModel = "NoSuchLens";
    a.lensfunFocalLengthMm = 50.0f;
    a.lensfunAperture = 4.0f;

    const bool ok = LensfunProfile::applyGeometry(a, pixels.data(), w, h);
    REQUIRE_FALSE(ok);
    REQUIRE(pixels == original);
}

TEST_CASE("Lensfun geometry leaves pixels unchanged on invalid capture params",
          "[lensfun]") {
    constexpr int w = 32;
    constexpr int h = 32;
    auto pixels = makePatternImage(w, h);
    const auto original = pixels;

    Adjustments a;
    a.lensfunEnabled = true;
    a.lensfunCameraMaker = "Canon";
    a.lensfunCameraModel = "EOS 5D Mark III";
    a.lensfunLensMaker = "Canon";
    a.lensfunLensModel = "EF 24-70mm f/2.8L II USM";
    a.lensfunFocalLengthMm = 0.0f;   // invalid: must short-circuit
    a.lensfunAperture = 0.0f;

    const bool ok = LensfunProfile::applyGeometry(a, pixels.data(), w, h);
    REQUIRE_FALSE(ok);
    REQUIRE(pixels == original);
}

TEST_CASE("Lensfun geometry remaps pixels for a known calibrated profile",
          "[lensfun]") {
    // A real camera/lens pair that ships with the Lensfun database and has a
    // distortion model. On a build without Lensfun or without the database,
    // applyGeometry returns false and the buffer is left untouched; on a
    // fully-equipped build it must move at least some pixels.
#if LRC_ENABLE_LENSFUN
    constexpr int w = 64;
    constexpr int h = 64;
    auto pixels = makePatternImage(w, h);
    const auto original = pixels;

    Adjustments a;
    a.lensfunEnabled = true;
    a.lensfunCameraMaker = "Canon";
    a.lensfunCameraModel = "Canon EOS 6D";
    a.lensfunLensMaker = "Canon";
    a.lensfunLensModel = "Canon EF 50mm f/1.8";
    a.lensfunFocalLengthMm = 50.0f;
    a.lensfunAperture = 1.8f;
    a.lensfunCropFactor = 1.0f;

    const bool ok = LensfunProfile::applyGeometry(a, pixels.data(), w, h);
    if (ok) {
        // The distortion model is non-zero, so the remap must diverge from the
        // identity at least somewhere away from the exact centre.
        bool changed = false;
        for (size_t i = 0; i < pixels.size() && !changed; ++i)
            changed = (pixels[i] != original[i]);
        REQUIRE(changed);
    } else {
        // No profile available on this machine: the safe no-op contract still
        // holds (buffer untouched).
        REQUIRE(pixels == original);
    }
#else
    SUCCEED("Lensfun disabled in this build; active path not exercised.");
#endif
}
