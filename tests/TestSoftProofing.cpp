// PHASE_15 — Soft proofing.
//
// Coverage (matches actual SoftProofing.h API — no RgbGamut / ColorSpace):
//   1. Default ctor produces no profile; hasProofProfile == false
//   2. loadProofProfile rejects non-existent paths; empty path clears profile
//   3. proofToDisplay without profile returns nullptr
//   4. With a valid lcms2 profile (we build a synthetic sRGB ICC via lcms2
//      in-engine if needed) proofToDisplay produces a U8 buffer matching dims
//   5. gamutMask returns an F32 grey buffer of matching dims
//   6. Multiple load / proof cycles are stable (no double-free / stale handle)

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "engine/ColorManager.h"
#include "engine/ImageBuffer.h"
#include "engine/SoftProofing.h"

namespace fs = std::filesystem;

namespace lrc::tests {

static ImageBuffer makeGradientF32(int w, int h) {
    ImageBuffer buf(w, h, PixelFormat::F32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float r = static_cast<float>(x) / std::max(1, w - 1);
            const float g = static_cast<float>(y) / std::max(1, h - 1);
            const float b = (r + g) * 0.5f;
            buf.setPixel(x, y, 0, r);
            buf.setPixel(x, y, 1, g);
            buf.setPixel(x, y, 2, b);
        }
    }
    return buf;
}

// Find any .icc or .icm profile shipped with the project / test fixtures so
// we don't rely on building a profile byte-by-byte.  Falls back to the
// lcms2 "sRGB built-in" profile path via the ColorManager.
static fs::path findAnyIccProfile() {
    // Search the workspace for any ICC/ICM file under resources/ or
    // third_party/lcms2/.  Finding none is fine — the first test still
    // exercises the "no profile" branch.
    const std::array<fs::path, 3> candidates = {
        "D:/Desktop/SilverHalide-src/resources",
        "D:/Desktop/SilverHalide-src/third_party/lcms2",
        fs::temp_directory_path(),
    };
    std::error_code ec;
    for (const auto& root : candidates) {
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(root, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const fs::path p = it->path();
            const auto ext = p.extension().string();
            bool iccLike = false;
            if (ext.size() == 4) {
                char e0 = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[1])));
                char e1 = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[2])));
                char e2 = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[3])));
                if ((e0 == 'i' && e1 == 'c' && (e2 == 'c' || e2 == 'm')))
                    iccLike = true;
            }
            if (iccLike) return p;
        }
    }
    return {};
}

TEST_CASE("SoftProofing: default ctor has no profile",
          "[ai][softproof]") {
    SoftProofing sp;
    CHECK_FALSE(sp.hasProofProfile());
    CHECK(sp.proofProfilePath().empty());
}

TEST_CASE("SoftProofing: loadProofProfile rejects missing paths",
          "[ai][softproof]") {
    SoftProofing sp;
    CHECK_FALSE(sp.loadProofProfile("__no_such_icc__.icc"));
    CHECK_FALSE(sp.hasProofProfile());
    // Empty path is a "clear" — returns true with hasProofProfile false.
    CHECK(sp.loadProofProfile({}));
    CHECK_FALSE(sp.hasProofProfile());
}

TEST_CASE("SoftProofing: proofToDisplay without profile returns nullptr",
          "[ai][softproof]") {
    SoftProofing sp;
    auto in = makeGradientF32(8, 4);
    auto out = sp.proofToDisplay(in, RenderingIntent::Perceptual, false);
    CHECK(out == nullptr);
    auto mask = sp.gamutMask(in, RenderingIntent::Perceptual);
    CHECK(mask == nullptr);
}

TEST_CASE("SoftProofing: with a valid ICC, proofToDisplay yields U8 same size",
          "[ai][softproof][lcms]") {
    const fs::path icc = findAnyIccProfile();
    if (icc.empty()) {
        SUCCEED("No ICC fixture found on this system — skipping proof branch.");
        return;
    }
    SoftProofing sp;
    REQUIRE(sp.loadProofProfile(icc));
    CHECK(sp.hasProofProfile());
    CHECK(sp.proofProfilePath() == icc);

    const int w = 16, h = 12;
    auto in = makeGradientF32(w, h);
    for (bool withGamut : {false, true}) {
        for (auto intent : {RenderingIntent::Perceptual,
                            RenderingIntent::RelativeColorimetric,
                            RenderingIntent::Saturation,
                            RenderingIntent::AbsoluteColorimetric}) {
            auto out = sp.proofToDisplay(in, intent, withGamut);
            REQUIRE(out != nullptr);
            CHECK(out->width() == w);
            CHECK(out->height() == h);
            CHECK(out->channels() == 3);
            // Display-ready proofing always comes back U8 sRGB.
            CHECK(out->format() == PixelFormat::U8);
        }
    }
}

TEST_CASE("SoftProofing: gamutMask yields F32 grey of matching dims",
          "[ai][softproof][lcms]") {
    const fs::path icc = findAnyIccProfile();
    if (icc.empty()) {
        SUCCEED("No ICC fixture found — skipping gamutMask branch.");
        return;
    }
    SoftProofing sp;
    REQUIRE(sp.loadProofProfile(icc));

    const int w = 16, h = 8;
    auto in = makeGradientF32(w, h);
    auto mask = sp.gamutMask(in, RenderingIntent::Perceptual);
    REQUIRE(mask != nullptr);
    CHECK(mask->width() == w);
    CHECK(mask->height() == h);
    CHECK(mask->channels() == 3);
    CHECK(mask->format() == PixelFormat::F32);

    // Mask must be a greyscale (R==G==B) field with values in [0,1].
    int bad = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float r = mask->pixel(x, y, 0);
            const float g = mask->pixel(x, y, 1);
            const float b = mask->pixel(x, y, 2);
            if (std::fabs(r - g) > 1e-4f) ++bad;
            if (std::fabs(g - b) > 1e-4f) ++bad;
            if (!std::isfinite(r)) ++bad;
            if (r < -0.001f || r > 1.001f) ++bad;
        }
    }
    CHECK(bad == 0);
}

TEST_CASE("SoftProofing: multiple load / proof cycles remain stable",
          "[ai][softproof][stress]") {
    const fs::path icc = findAnyIccProfile();
    SoftProofing sp;
    const int w = 8, h = 4;
    auto in = makeGradientF32(w, h);

    for (int i = 0; i < 6; ++i) {
        if (!icc.empty() && (i % 2 == 0)) REQUIRE(sp.loadProofProfile(icc));
        else                              REQUIRE(sp.loadProofProfile({}));

        auto out = sp.proofToDisplay(in, RenderingIntent::Perceptual, (i % 2) == 1);
        auto mask = sp.gamutMask(in, RenderingIntent::RelativeColorimetric);
        if (sp.hasProofProfile()) {
            REQUIRE(out != nullptr);
            CHECK(out->width() == w);
            CHECK(out->height() == h);
            REQUIRE(mask != nullptr);
            CHECK(mask->width() == w);
            CHECK(mask->height() == h);
        } else {
            CHECK(out == nullptr);
            CHECK(mask == nullptr);
        }
    }
}

TEST_CASE("SoftProofing: in-memory ICC blob loads and proofs",
          "[ai][softproof][lcms]") {
    // The UI's built-in destinations (sRGB / Adobe RGB / Display P3) are
    // serialized in memory by ColorManager rather than shipped as .icc files, so
    // this is the exact path the "视图 → 软打样…" dialog relies on. Using the
    // generated bytes keeps the test independent of any fixture on disk.
    const auto bytes = ColorManager::exportProfileBytes(
        ColorManager::ExportColorProfile::SRGB);
    REQUIRE_FALSE(bytes.empty());

    SoftProofing sp;
    REQUIRE(sp.loadProofProfileFromMemory(bytes));
    CHECK(sp.hasProofProfile());
    // A memory-loaded profile has no file path behind it.
    CHECK(sp.proofProfilePath().empty());

    const int w = 16, h = 12;
    auto in = makeGradientF32(w, h);
    auto out = sp.proofToDisplay(in, RenderingIntent::Perceptual, false);
    REQUIRE(out != nullptr);
    CHECK(out->width() == w);
    CHECK(out->height() == h);
    CHECK(out->channels() == 3);
    CHECK(out->format() == PixelFormat::U8);
}

TEST_CASE("SoftProofing: every in-memory preset colour space proofs",
          "[ai][softproof][lcms]") {
    const int w = 8, h = 6;
    auto in = makeGradientF32(w, h);
    for (auto preset : {ColorManager::ExportColorProfile::SRGB,
                        ColorManager::ExportColorProfile::AdobeRGB,
                        ColorManager::ExportColorProfile::DisplayP3}) {
        const auto bytes = ColorManager::exportProfileBytes(preset);
        REQUIRE_FALSE(bytes.empty());
        SoftProofing sp;
        REQUIRE(sp.loadProofProfileFromMemory(bytes));
        CHECK(sp.hasProofProfile());
        auto out = sp.proofToDisplay(in, RenderingIntent::RelativeColorimetric,
                                     true);
        REQUIRE(out != nullptr);
        CHECK(out->width() == w);
        CHECK(out->height() == h);
    }
}

TEST_CASE("SoftProofing: empty blob clears, garbage blob fails safely",
          "[ai][softproof]") {
    const auto bytes = ColorManager::exportProfileBytes(
        ColorManager::ExportColorProfile::SRGB);
    REQUIRE_FALSE(bytes.empty());

    SoftProofing sp;
    REQUIRE(sp.loadProofProfileFromMemory(bytes));
    REQUIRE(sp.hasProofProfile());

    // Empty bytes == "clear the profile" (same contract as an empty path).
    CHECK(sp.loadProofProfileFromMemory({}));
    CHECK_FALSE(sp.hasProofProfile());

    // A blob that is not an ICC profile must fail and leave no profile behind.
    const std::vector<uint8_t> garbage(64, 0x7f);
    CHECK_FALSE(sp.loadProofProfileFromMemory(garbage));
    CHECK_FALSE(sp.hasProofProfile());

    // The engine must still be usable afterwards.
    REQUIRE(sp.loadProofProfileFromMemory(bytes));
    auto out = sp.proofToDisplay(makeGradientF32(4, 4),
                                 RenderingIntent::Perceptual, false);
    REQUIRE(out != nullptr);
    CHECK(out->width() == 4);
}

// --- Out-of-gamut alarm colour ----------------------------------------------
//
// lcms marks every out-of-gamut pixel with an "alarm code" instead of its proofed
// value; that colour is what the user sees as the gamut warning. It used to be
// hard-coded pure red (set on lcms' *process-global* context by the ctor), so the
// dialog's colour picker only affected the exported gamut report and
// gamutMask() — which recognises the marked pixels by matching that colour —
// silently went empty for any other colour.

namespace {

// Saturated linear ProPhoto primaries/secondaries. ProPhoto is the widest space
// the pipeline works in, so these are far outside an sRGB destination and the
// gamut-check leg must mark at least one of them.
ImageBuffer makeWideGamutRow() {
    const std::array<std::array<float, 3>, 6> colours = {{
        {{1.0f, 0.0f, 0.0f}}, {{0.0f, 1.0f, 0.0f}}, {{0.0f, 0.0f, 1.0f}},
        {{1.0f, 1.0f, 0.0f}}, {{0.0f, 1.0f, 1.0f}}, {{0.25f, 0.25f, 0.25f}},
    }};
    ImageBuffer buf(static_cast<int>(colours.size()), 1, PixelFormat::F32);
    for (int x = 0; x < static_cast<int>(colours.size()); ++x) {
        buf.setPixel(x, 0, 0, colours[static_cast<size_t>(x)][0]);
        buf.setPixel(x, 0, 1, colours[static_cast<size_t>(x)][1]);
        buf.setPixel(x, 0, 2, colours[static_cast<size_t>(x)][2]);
    }
    return buf;
}

// Indices of the mask's marked pixels (1.0 = out of gamut).
std::vector<int> markedPixels(const ImageBuffer& mask) {
    std::vector<int> out;
    for (int x = 0; x < mask.width(); ++x)
        if (mask.pixel(x, 0, 0) > 0.5f)
            out.push_back(x);
    return out;
}

bool pixelIs(const ImageBuffer& proof, int x, uint8_t r, uint8_t g, uint8_t b) {
    const auto ch = [&](int c) {
        return static_cast<int>(
            std::lround(std::clamp(proof.pixel(x, 0, c), 0.0f, 1.0f) * 255.0f));
    };
    return ch(0) == r && ch(1) == g && ch(2) == b;
}

} // namespace

TEST_CASE("SoftProofing: the alarm colour drives both the proof frame and mask",
          "[ai][softproof][lcms]") {
    const auto bytes = ColorManager::exportProfileBytes(
        ColorManager::ExportColorProfile::SRGB);
    REQUIRE_FALSE(bytes.empty());

    const auto in = makeWideGamutRow();
    SoftProofing sp;
    REQUIRE(sp.loadProofProfileFromMemory(bytes));

    // Default is still pure red.
    const GamutAlarmCodes def = sp.gamutAlarmCodes();
    CHECK(def.r == 255);
    CHECK(def.g == 0);
    CHECK(def.b == 0);

    auto redProof =
        sp.proofToDisplay(in, RenderingIntent::RelativeColorimetric, true);
    REQUIRE(redProof != nullptr);
    auto redMask = sp.gamutMask(in, RenderingIntent::RelativeColorimetric);
    REQUIRE(redMask != nullptr);
    const std::vector<int> marked = markedPixels(*redMask);
    REQUIRE_FALSE(marked.empty());  // the fixture really is out of gamut
    for (int x : marked)
        CHECK(pixelIs(*redProof, x, 255, 0, 0));

    // Recolouring must affect the proof frame *and* keep the mask intact. The
    // transform is cached and reused here, which is exactly right: lcms reads the
    // codes from the context on every run, so no rebuild is needed.
    sp.setGamutAlarmCodes(0, 255, 0);
    CHECK(sp.gamutAlarmCodes().g == 255);

    auto greenProof =
        sp.proofToDisplay(in, RenderingIntent::RelativeColorimetric, true);
    REQUIRE(greenProof != nullptr);
    for (int x : marked)
        CHECK(pixelIs(*greenProof, x, 0, 255, 0));

    auto greenMask = sp.gamutMask(in, RenderingIntent::RelativeColorimetric);
    REQUIRE(greenMask != nullptr);
    // The regression: with a non-red colour this used to come back empty.
    CHECK(markedPixels(*greenMask) == marked);
}

TEST_CASE("SoftProofing: the alarm colour is per-instance, not process-global",
          "[ai][softproof][lcms]") {
    // Two instances must not recolour each other's transforms: the codes live in
    // each instance's private lcms context (they used to be written to the global
    // one, so the last setter won for every transform in the process — including
    // an export running on another thread).
    const auto bytes = ColorManager::exportProfileBytes(
        ColorManager::ExportColorProfile::SRGB);
    REQUIRE_FALSE(bytes.empty());

    const auto in = makeWideGamutRow();
    SoftProofing red;
    SoftProofing blue;
    REQUIRE(red.loadProofProfileFromMemory(bytes));
    REQUIRE(blue.loadProofProfileFromMemory(bytes));
    red.setGamutAlarmCodes(255, 0, 0);
    blue.setGamutAlarmCodes(0, 0, 255);

    auto redMask = red.gamutMask(in, RenderingIntent::RelativeColorimetric);
    auto blueMask = blue.gamutMask(in, RenderingIntent::RelativeColorimetric);
    REQUIRE(redMask != nullptr);
    REQUIRE(blueMask != nullptr);
    const std::vector<int> marked = markedPixels(*redMask);
    REQUIRE_FALSE(marked.empty());
    CHECK(markedPixels(*blueMask) == marked);

    auto redProof =
        red.proofToDisplay(in, RenderingIntent::RelativeColorimetric, true);
    auto blueProof =
        blue.proofToDisplay(in, RenderingIntent::RelativeColorimetric, true);
    REQUIRE(redProof != nullptr);
    REQUIRE(blueProof != nullptr);
    // Interleaved use: each instance keeps its own colour.
    for (int x : marked) {
        CHECK(pixelIs(*redProof, x, 255, 0, 0));
        CHECK(pixelIs(*blueProof, x, 0, 0, 255));
    }
}

} // namespace lrc::tests
