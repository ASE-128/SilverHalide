// PHASE_13 — AI mask generator.
//
// Coverage:
//   1. generate returns a valid F32 ImageBuffer of shape (w, h, 3) where the
//      weight is stored as R=G=B (saturated grey).  Values in [0,1].
//   2. invert flag flips weight to 1-weight.
//   3. Increasing threshold strictly shrinks the fraction of pixels above 0.5.
//   4. Sky heuristic prefers the top half of the scene (blue band test).
//   5. AiMaskParams defaults & Mask hasEffect semantics.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "engine/Adjustments.h"
#include "engine/AiMaskGenerator.h"
#include "engine/ImageBuffer.h"
#include "engine/Pipeline.h"

namespace lrc::tests {

static ImageBuffer makeSyntheticScene(int w, int h) {
    ImageBuffer buf(w, h, PixelFormat::F32);
    const int horizonY = h / 3;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float r, g, b;
            if (y < horizonY) {
                r = 0.42f; g = 0.6f; b = 0.95f;
            } else {
                r = 0.18f; g = 0.22f; b = 0.1f;
            }
            const int cx = w / 2, cy = h * 2 / 3;
            const int dx = x - cx, dy = y - cy;
            const float d2 = static_cast<float>(dx*dx + dy*dy);
            const float radius = std::min(w, h) * 0.15f;
            if (d2 < radius * radius) {
                r = 0.92f; g = 0.75f; b = 0.3f;
            }
            buf.setPixel(x, y, 0, r);
            buf.setPixel(x, y, 1, g);
            buf.setPixel(x, y, 2, b);
        }
    }
    return buf;
}

static float fractionAbove(const ImageBuffer& grey, float t) {
    // Mask generator writes weight R=G=B, so sampling channel 0 is enough.
    int cnt = 0;
    const int total = grey.width() * grey.height();
    for (int i = 0; i < total; ++i) {
        const float v = reinterpret_cast<const float*>(grey.data().data())[3 * i];
        if (v > t) ++cnt;
    }
    return static_cast<float>(cnt) / static_cast<float>(std::max(1, total));
}

TEST_CASE("AiMaskGenerator: fallback returns valid F32 grey buffer",
          "[ai][mask]") {
    const int w = 32, h = 24;
    auto scene = makeSyntheticScene(w, h);

    AiMaskParams params;
    params.target = AiMaskTarget::Subject;
    AiMaskGenerator gen(nullptr);
    auto out = gen.generate(scene, params);
    REQUIRE(out);
    CHECK(out->width() == w);
    CHECK(out->height() == h);
    CHECK(out->format() == PixelFormat::F32);
    CHECK(out->channels() == 3);

    float mn = 2.0f, mx = -1.0f;
    int bad = 0;
    for (int i = 0; i < w * h; ++i) {
        const float r = reinterpret_cast<const float*>(out->data().data())[3*i + 0];
        const float g = reinterpret_cast<const float*>(out->data().data())[3*i + 1];
        const float b = reinterpret_cast<const float*>(out->data().data())[3*i + 2];
        if (std::fabs(r - g) > 1e-4f || std::fabs(g - b) > 1e-4f) ++bad;
        if (std::isnan(r)) ++bad;
        mn = std::min(mn, r);
        mx = std::max(mx, r);
    }
    // Greyscale consistency check: every pixel's R==G==B to within epsilon.
    CHECK(bad == 0);
    CHECK(mn >= -0.001f);
    CHECK(mx <= 1.001f);
}

TEST_CASE("AiMaskGenerator: invert flag flips output grey level",
          "[ai][mask]") {
    const int w = 32, h = 24;
    auto scene = makeSyntheticScene(w, h);
    AiMaskGenerator gen(nullptr);

    AiMaskParams pA;
    pA.target = AiMaskTarget::Subject;
    pA.invert = false;
    auto outA = gen.generate(scene, pA);
    AiMaskParams pB = pA;
    pB.invert = true;
    auto outB = gen.generate(scene, pB);
    REQUIRE(outA);
    REQUIRE(outB);

    double sumAbsDiff = 0.0;
    for (int i = 0; i < w * h * 3; ++i) {
        const float a = reinterpret_cast<const float*>(outA->data().data())[i];
        const float b = reinterpret_cast<const float*>(outB->data().data())[i];
        sumAbsDiff += std::fabs((1.0f - a) - b);
    }
    const double mean = sumAbsDiff / (w * h * 3.0);
    CHECK(mean < 1e-4);
}

TEST_CASE("AiMaskGenerator: increasing threshold shrinks hot mask pixels",
          "[ai][mask]") {
    const int w = 48, h = 32;
    auto scene = makeSyntheticScene(w, h);
    AiMaskGenerator gen(nullptr);

    AiMaskParams p;
    p.target = AiMaskTarget::Subject;
    p.feather = 0.0f;

    float fractions[3];
    for (int i = 0; i < 3; ++i) {
        p.threshold = static_cast<float>(i) * 0.25f; // 0, 0.25, 0.5
        auto out = gen.generate(scene, p);
        REQUIRE(out);
        fractions[i] = fractionAbove(*out, 0.5f);
    }
    CHECK(fractions[0] + 1e-4f >= fractions[1]);
    CHECK(fractions[1] + 1e-4f >= fractions[2]);
}

TEST_CASE("AiMaskGenerator: Sky heuristic prefers top half over bottom half",
          "[ai][mask]") {
    const int w = 48, h = 36;
    auto scene = makeSyntheticScene(w, h);
    AiMaskGenerator gen(nullptr);

    AiMaskParams p;
    p.target = AiMaskTarget::Sky;
    p.threshold = 0.0f;
    p.feather = 0.0f;
    auto out = gen.generate(scene, p);
    REQUIRE(out);

    auto rowAvg = [&](int y) {
        double s = 0.0;
        for (int x = 0; x < w; ++x) s += out->pixel(x, y, 0);
        return s / w;
    };

    double top = 0.0, bottom = 0.0;
    for (int y = 0; y < h / 2; ++y) top += rowAvg(y);
    for (int y = h / 2; y < h; ++y) bottom += rowAvg(y);
    top /= (h / 2.0);
    bottom /= (h - h / 2.0);
    CHECK(top > bottom);
}

TEST_CASE("AiMaskParams: defaults produce a sane subject-mask config",
          "[ai][mask][adjustments]") {
    AiMaskParams p;
    CHECK(p.target == AiMaskTarget::Subject);
    CHECK(p.threshold == 0.5f);
    CHECK(p.feather == 0.05f);
    CHECK(p.modelInputSize == 1024);
    CHECK(p.invert == false);
    CHECK(p.cachedWeightVersion == 0);
    CHECK(p.modelIdOverride.empty());
}

TEST_CASE("Mask::hasEffect considers MaskType::AiMask",
          "[ai][mask][adjustments]") {
    Mask m;
    CHECK_FALSE(m.hasEffect());
    m.type = MaskType::AiMask;
    CHECK(m.hasEffect());
    m.opacity = 0.0;
    CHECK_FALSE(m.hasEffect());
}

// The viewport overlay and the render must agree, so both go through
// Pipeline::aiMaskWeightField. It used to be impossible to reach the AI weight
// at all outside a build with a loaded model, and the UI compensated by
// painting a constant 0.5 placeholder over the whole frame — which read as
// "everything is selected". These cases pin the real, content-driven field.
TEST_CASE("Pipeline::aiMaskWeightField is content-driven, not a constant",
          "[ai][mask][pipeline]") {
    constexpr int kWidth = 64;
    constexpr int kHeight = 48;
    const ImageBuffer scene = makeSyntheticScene(kWidth, kHeight);

    Mask m;
    m.type = MaskType::AiMask;
    m.ai.target = AiMaskTarget::Sky;
    m.ai.cachedWeightVersion = 1;  // distinct from a default-constructed mask

    const auto field = Pipeline::aiMaskWeightField(m, scene);
    REQUIRE(field != nullptr);
    REQUIRE(field->size() ==
            static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight));

    // A constant field is exactly the bug: the old placeholder was 0.5
    // everywhere, which carries no information about the image at all.
    const float first = (*field)[0];
    const bool allEqual = std::all_of(
        field->begin(), field->end(),
        [first](float v) { return std::fabs(v - first) < 1e-6f; });
    CHECK_FALSE(allEqual);

    // The sky heuristic keys on the upper part of the frame, so the top third
    // must outweigh the bottom third for a synthetic sky/ground scene.
    const int horizon = kHeight / 3;
    double topSum = 0.0, bottomSum = 0.0;
    for (int y = 0; y < horizon; ++y)
        for (int x = 0; x < kWidth; ++x)
            topSum += (*field)[static_cast<size_t>(y) * kWidth + x];
    for (int y = 2 * horizon; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
            bottomSum += (*field)[static_cast<size_t>(y) * kWidth + x];
    CHECK(topSum / horizon > bottomSum / (kHeight - 2 * horizon));
}

TEST_CASE("Pipeline::aiMaskWeightField rejects non-AI masks",
          "[ai][mask][pipeline]") {
    const ImageBuffer scene = makeSyntheticScene(32, 24);

    Mask linear;
    linear.type = MaskType::LinearGradient;
    CHECK(Pipeline::aiMaskWeightField(linear, scene) == nullptr);

    Mask none;
    CHECK(Pipeline::aiMaskWeightField(none, scene) == nullptr);
}

} // namespace lrc::tests
