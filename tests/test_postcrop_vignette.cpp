// PHASE_3 post-crop vignette tests.
//
// Covers:
//   - The vignette weight field is centred on the render buffer's geometric
//     centre and stays centred when the buffer size / aspect changes.
//   - The shape follows roundness (-100 rectangular, +100 circular) and the
//     midpoint / feather sliders move and soften the falloff.
//   - The highlights slider protects bright pixels from the darkening.
//   - XMP shape fields round-trip (non-default values write and read back).
//   - The all-default fast path leaves the buffer untouched.
//   - The weight field is cached: a pure amount change reuses the cached field,
//     and changing only a shape parameter recomputes just stage E incrementally.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/Adjustments.h"
#include "engine/ColorManager.h"
#include "engine/ImageBuffer.h"
#include "engine/Pipeline.h"
#include "utils/ProfileIO.h"

using namespace Catch;
using namespace lrc;

namespace {

// Reads the vignette weight at (x,y) from a cached field (entry y*w+x).
float weightAt(const std::shared_ptr<const std::vector<float>>& field, int w,
               int x, int y) {
    return (*field)[static_cast<size_t>(y) * w + x];
}

} // namespace

TEST_CASE("post-crop vignette: weight field is centred on the buffer geometric "
          "centre", "[vignette]") {
    Adjustments a;
    a.vignette = -50.0;  // amount is irrelevant to the weight field

    for (int w : {65, 100, 128}) {
        for (int h : {65, 50, 80}) {
            auto field = Pipeline::postCropVignetteWeightField(a, w, h);
            REQUIRE(field);
            REQUIRE(field->size() == static_cast<size_t>(w) * h);

            // Symmetry about the geometric centre: the field at (x,y) equals
            // the field at the mirrored pixel (w-1-x, h-1-y). This is exact
            // because the distance formula is built from |nx|, |ny| with the
            // centre at ((w-1)/2, (h-1)/2).
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const float lhs = weightAt(field, w, x, y);
                    const float rhs =
                        weightAt(field, w, w - 1 - x, h - 1 - y);
                    REQUIRE(lhs == rhs);
                }
            }

            // The vignette's centre is the geometric centre: with the default
            // midpoint/feather the falloff starts at d = 0.25, so the weight at
            // the centre pixel is exactly 0.
            const int cx = (w - 1) / 2;
            const int cy = (h - 1) / 2;
            REQUIRE(weightAt(field, w, cx, cy) == 0.0f);
        }
    }
}

TEST_CASE("post-crop vignette: roundness shapes the falloff (rectangle vs "
          "circle)", "[vignette]") {
    const int w = 100, h = 100;
    // A late, narrow falloff so the mid-edge and corner weights differ clearly.
    Adjustments rect;
    rect.vignette = -50.0;
    rect.vignetteMidpoint = 70.0;
    rect.vignetteFeather = 20.0;
    rect.vignetteRoundness = -100.0;  // rectangle
    Adjustments circ = rect;
    circ.vignetteRoundness = 100.0;   // circle

    auto rectField = Pipeline::postCropVignetteWeightField(rect, w, h);
    auto circField = Pipeline::postCropVignetteWeightField(circ, w, h);

    const int cx = (w - 1) / 2;  // 49
    const float rectEdge = weightAt(rectField, w, w - 1, cx);   // mid-edge
    const float circEdge = weightAt(circField, w, w - 1, cx);
    const float rectCorner = weightAt(rectField, w, 0, 0);
    const float circCorner = weightAt(circField, w, 0, 0);

    // Rectangle: mid-edge and corner both reach full strength (distance 1).
    REQUIRE(rectEdge == Approx(1.0f).margin(1e-4f));
    REQUIRE(rectCorner == Approx(1.0f).margin(1e-4f));
    // Circle: the corner is farther from the centre than the mid-edge, so the
    // corner weight exceeds the mid-edge weight.
    REQUIRE(circCorner > circEdge + 0.2f);
}

TEST_CASE("post-crop vignette: midpoint and feather control the falloff",
          "[vignette]") {
    const int w = 65, h = 65;
    const int cy = (w - 1) / 2;  // 32
    // A pixel ~59% of the way from the centre to the right edge.
    const int px = 51, py = cy;

    Adjustments base;
    base.vignette = -50.0;

    // Low midpoint (30): the falloff centre is near the centre, so this pixel
    // sits past the transition and gets full weight.
    Adjustments low = base;
    low.vignetteMidpoint = 30.0;
    low.vignetteFeather = 20.0;   // transition [0.2, 0.4]
    auto lowField = Pipeline::postCropVignetteWeightField(low, w, h);
    const float wLow = weightAt(lowField, w, px, py);

    // High midpoint (70): the falloff is pushed to the edge, so this pixel is
    // well inside the untouched region.
    Adjustments high = base;
    high.vignetteMidpoint = 70.0;
    high.vignetteFeather = 20.0;  // transition [0.6, 0.8]
    auto highField = Pipeline::postCropVignetteWeightField(high, w, h);
    const float wHigh = weightAt(highField, w, px, py);

    REQUIRE(wLow > 0.8f);
    REQUIRE(wHigh < 0.2f);

    // Feather: a wide transition (100) makes the weight ramp up slowly, so the
    // same pixel that is saturated under a narrow transition reads partial.
    Adjustments soft = low;
    soft.vignetteFeather = 100.0;  // transition [0.0, 1.0]
    auto softField = Pipeline::postCropVignetteWeightField(soft, w, h);
    const float wSoft = weightAt(softField, w, px, py);
    REQUIRE(wSoft > 0.0f);
    REQUIRE(wSoft < wLow);
}

TEST_CASE("post-crop vignette: highlights protects bright pixels from the "
          "darkening", "[vignette]") {
    const int w = 65, h = 65;
    auto makeBuf = [&]() {
        return std::vector<float>(static_cast<size_t>(w) * h * 3, 0.5f);
    };
    auto at = [&](const std::vector<float>& buf, int x, int y) {
        return buf[(static_cast<size_t>(y) * w + x) * 3];
    };
    const size_t darkIdx = (static_cast<size_t>(h - 1) * w + 0) * 3;

    // Baseline: highlights = 0 -> both symmetric corners are darkened equally
    // (the weight at both corners is 1, so -100 amount drives them to ~0).
    {
        std::vector<float> buf = makeBuf();
        buf[0] = buf[1] = buf[2] = 0.9f;                        // bright (0,0)
        buf[darkIdx] = buf[darkIdx + 1] = buf[darkIdx + 2] = 0.1f;  // dark (0,h-1)
        Adjustments a;
        a.vignette = -100.0;
        a.vignetteHighlights = 0.0;
        Pipeline::applySpatialAdjustments(a, buf.data(), w, h);
        REQUIRE(at(buf, 0, 0) < 0.05f);
        REQUIRE(at(buf, 0, h - 1) < 0.05f);
    }

    // With highlight protection, the bright pixel keeps most of its value while
    // the dark pixel is still crushed.
    {
        std::vector<float> buf = makeBuf();
        buf[0] = buf[1] = buf[2] = 0.9f;
        buf[darkIdx] = buf[darkIdx + 1] = buf[darkIdx + 2] = 0.1f;
        Adjustments a;
        a.vignette = -100.0;
        a.vignetteHighlights = 100.0;
        Pipeline::applySpatialAdjustments(a, buf.data(), w, h);
        REQUIRE(at(buf, 0, 0) > 0.5f);
        REQUIRE(at(buf, 0, h - 1) < 0.05f);
    }
}

TEST_CASE("post-crop vignette: ProfileIO JSON shape fields round-trip",
          "[vignette]") {
    Adjustments a;
    a.vignette = -30.0;
    a.vignetteMidpoint = 35.0;
    a.vignetteRoundness = 40.0;
    a.vignetteFeather = 70.0;
    a.vignetteHighlights = 25.0;

    const auto value = ProfileIO::toJson(a);
    REQUIRE(value.contains("vignette"));
    REQUIRE(value.contains("vignetteMidpoint"));
    REQUIRE(value.contains("vignetteRoundness"));
    REQUIRE(value.contains("vignetteFeather"));
    REQUIRE(value.contains("vignetteHighlights"));

    const Adjustments r = ProfileIO::fromJson(value);
    REQUIRE(r.vignette == Approx(-30.0).margin(1e-6));
    REQUIRE(r.vignetteMidpoint == Approx(35.0).margin(1e-6));
    REQUIRE(r.vignetteRoundness == Approx(40.0).margin(1e-6));
    REQUIRE(r.vignetteFeather == Approx(70.0).margin(1e-6));
    REQUIRE(r.vignetteHighlights == Approx(25.0).margin(1e-6));
}

TEST_CASE("post-crop vignette: all-default parameters are a no-op",
          "[vignette]") {
    const int w = 65, h = 65;
    std::vector<float> px(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < px.size(); ++i)
        px[i] = 0.3f + 0.4f * static_cast<float>((i / 3) % 7) / 6.0f;
    const auto input = px;

    Pipeline::applySpatialAdjustments(Adjustments::defaults(), px.data(), w, h);
    // Bit-identical: every spatial stage is neutral and the final clamp is the
    // identity for in-range inputs.
    REQUIRE(px == input);
}

TEST_CASE("post-crop vignette: weight field is cached and reused on a pure "
          "amount change", "[vignette]") {
    Adjustments a;
    a.vignette = -50.0;
    a.vignetteMidpoint = 40.0;
    a.vignetteRoundness = 10.0;
    a.vignetteFeather = 60.0;

    auto f1 = Pipeline::postCropVignetteWeightField(a, 100, 60);

    // Same shape, different amount: the cached field is reused (same object).
    Adjustments b = a;
    b.vignette = -90.0;
    auto f2 = Pipeline::postCropVignetteWeightField(b, 100, 60);
    REQUIRE(f2.get() == f1.get());
    REQUIRE(*f2 == *f1);

    // A shape change (midpoint) produces a different field.
    Adjustments c = a;
    c.vignetteMidpoint = 80.0;
    auto f3 = Pipeline::postCropVignetteWeightField(c, 100, 60);
    REQUIRE(f3.get() != f1.get());
    REQUIRE(*f3 != *f1);

    // A different buffer size also produces a different field.
    auto f4 = Pipeline::postCropVignetteWeightField(a, 120, 60);
    REQUIRE(f4.get() != f1.get());
}

TEST_CASE("post-crop vignette: changing a shape field recomputes only stage E",
          "[vignette][incremental]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);

    const int w = 64, h = 40;
    auto src = ImageBuffer::create(w, h, PixelFormat::F32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float v = static_cast<float>(x + y) /
                            static_cast<float>(w + h);
            src->setPixel(x, y, 0, v);
            src->setPixel(x, y, 1, v * 0.7f);
            src->setPixel(x, y, 2, v * 0.4f);
        }
    const Size target{w, h};
    constexpr uint64_t imageId = 9001;

    Adjustments base;
    base.vignette = -40.0;  // active vignette in stage E
    auto r1 = pipeline.renderPreview(*src, base, target, w, imageId);
    REQUIRE((r1 && !r1->empty()));
    REQUIRE(pipeline.lastRecomputeStage() == 0);

    // Change only the midpoint: stage E recomputes, reusing the cached stage D.
    Adjustments mid = base;
    mid.vignetteMidpoint = 70.0;
    auto r2 = pipeline.renderPreview(*src, mid, target, w, imageId);
    REQUIRE((r2 && !r2->empty()));
    REQUIRE(pipeline.lastRecomputeStage() == 4);
    REQUIRE(pipeline.lastReuseStartStage() == 3);

    // Change only the amount: stage E recomputes (the weight field itself is
    // reused via the global vignette cache).
    Adjustments amt = base;
    amt.vignette = -80.0;
    auto r3 = pipeline.renderPreview(*src, amt, target, w, imageId);
    REQUIRE((r3 && !r3->empty()));
    REQUIRE(pipeline.lastRecomputeStage() == 4);
    REQUIRE(pipeline.lastReuseStartStage() == 3);

    // Identical adjustments are served entirely from the cache.
    auto r4 = pipeline.renderPreview(*src, amt, target, w, imageId);
    REQUIRE(r4.get() == r3.get());
    REQUIRE(pipeline.lastRecomputeStage() == -1);
}
