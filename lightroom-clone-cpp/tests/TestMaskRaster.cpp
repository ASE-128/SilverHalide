#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <vector>

#include "engine/Adjustments.h"
#include "engine/Pipeline.h"

using namespace Catch;
using namespace lrc;

namespace {

// Serial reference for Pipeline::applyLocalAdjustments that builds the weight
// per pixel via Pipeline::maskWeight (the sparse O(D) API) instead of the
// rasterized + cached brushWeightField. The local-adjustment gains mirror
// Pipeline.cpp exactly (clarity excluded: its blurred-luma path is file-local
// and out of scope for this comparison). Proves the field path is consistent
// with the per-pixel API the pipeline used before the rasterization change.
void referenceApplyLocalAdjustments(const Adjustments& a, float* p, int w,
                                    int h) {
    const size_t n = static_cast<size_t>(w) * h;
    std::vector<float> weight(n);
    for (const Mask& m : a.masks) {
        if (m.type == MaskType::None || m.opacity <= 0.0 || !m.hasEffect())
            continue;
        const float opacity =
            static_cast<float>(std::clamp(m.opacity, 0.0, 1.0));

        for (int y = 0; y < h; ++y) {
            const float ny = (h > 1) ? static_cast<float>(y) / (h - 1) : 0.0f;
            for (int x = 0; x < w; ++x) {
                const float nx =
                    (w > 1) ? static_cast<float>(x) / (w - 1) : 0.0f;
                const size_t i = static_cast<size_t>(y) * w + x;
                weight[i] = Pipeline::maskWeight(m, w, h, nx, ny) * opacity;
            }
        }

        const float exposure = static_cast<float>(std::pow(2.0, m.exposure));
        const float cscale =
            1.0f + static_cast<float>(m.contrast) / 100.0f * 0.7f;
        const float highlightsGain =
            1.0f + static_cast<float>(m.highlights) / 100.0f * 0.5f;
        const float shadowsGain =
            1.0f + static_cast<float>(m.shadows) / 100.0f * 0.5f;
        const float wGain = static_cast<float>(m.whites) / 100.0f * 0.9f;
        const float bGain = static_cast<float>(m.blacks) / 100.0f * 0.9f;
        const float rTemp =
            static_cast<float>(std::pow(2.0, m.temperature * 0.006));
        const float bTemp =
            static_cast<float>(std::pow(2.0, -m.temperature * 0.006));
        const float gTint =
            static_cast<float>(std::pow(2.0, m.tint * 0.006));
        const float dhBoost =
            1.0f + static_cast<float>(m.dehaze) / 100.0f * 1.4f;
        constexpr float kHazeFloor = 0.12f;
        const float satScale =
            1.0f + static_cast<float>(m.saturation) / 100.0f;

        auto smooth = [](float x) -> float {
            if (x <= 0.0f) return 0.0f;
            if (x >= 1.0f) return 1.0f;
            return x * x * (3.0f - 2.0f * x);
        };

        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                const float wgt = weight[i];
                if (wgt <= 1e-4f)
                    continue;
                float* px = p + i * 3;
                const float or0 = px[0], or1 = px[1], or2 = px[2];

                float r = or0, g = or1, b = or2;

                r *= exposure;
                g *= exposure;
                b *= exposure;

                r = 0.18f + (r - 0.18f) * cscale;
                g = 0.18f + (g - 0.18f) * cscale;
                b = 0.18f + (b - 0.18f) * cscale;

                const float hw = smooth((r - 0.35f) / 0.65f);
                const float hw_g = smooth((g - 0.35f) / 0.65f);
                const float hw_b = smooth((b - 0.35f) / 0.65f);
                r *= 1.0f + (highlightsGain - 1.0f) * hw;
                g *= 1.0f + (highlightsGain - 1.0f) * hw_g;
                b *= 1.0f + (highlightsGain - 1.0f) * hw_b;
                const float sw = smooth((0.35f - r) / 0.35f);
                const float sw_g = smooth((0.35f - g) / 0.35f);
                const float sw_b = smooth((0.35f - b) / 0.35f);
                r *= 1.0f + (shadowsGain - 1.0f) * sw;
                g *= 1.0f + (shadowsGain - 1.0f) * sw_g;
                b *= 1.0f + (shadowsGain - 1.0f) * sw_b;

                if (m.whites != 0.0 || m.blacks != 0.0) {
                    r += (r - 0.18f) * wGain * smooth((r - 0.18f) / 0.82f);
                    g += (g - 0.18f) * wGain * smooth((g - 0.18f) / 0.82f);
                    b += (b - 0.18f) * wGain * smooth((b - 0.18f) / 0.82f);
                    r -= (0.18f - r) * bGain * smooth((0.18f - r) / 0.18f);
                    g -= (0.18f - g) * bGain * smooth((0.18f - g) / 0.18f);
                    b -= (0.18f - b) * bGain * smooth((0.18f - b) / 0.18f);
                }

                r *= rTemp;
                b *= bTemp;
                g *= gTint;

                if (m.dehaze != 0.0) {
                    const float floor = kHazeFloor * (1.0f - dhBoost);
                    r = std::max(floor + (r - floor) * dhBoost, 0.0f);
                    g = std::max(floor + (g - floor) * dhBoost, 0.0f);
                    b = std::max(floor + (b - floor) * dhBoost, 0.0f);
                }

                if (m.saturation != 0.0) {
                    const float ls = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    r = ls + (r - ls) * satScale;
                    g = ls + (g - ls) * satScale;
                    b = ls + (b - ls) * satScale;
                }

                px[0] = or0 + (r - or0) * wgt;
                px[1] = or1 + (g - or1) * wgt;
                px[2] = or2 + (b - or2) * wgt;
            }
    }
}

} // namespace

TEST_CASE("MaskRaster: brushWeightField matches maskWeight at sample points",
          "[mask][raster]") {
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);

    for (int trial = 0; trial < 12; ++trial) {
        Mask m;
        m.type = MaskType::Brush;
        const int ndabs = 1 + static_cast<int>(rng() % 6);
        for (int d = 0; d < ndabs; ++d) {
            BrushDab dab;
            dab.x = u01(rng);
            dab.y = u01(rng);
            dab.radius = 0.02f + 0.35f * u01(rng);
            dab.softness = u01(rng);
            m.brush.push_back(dab);
        }
        m.invert = (rng() & 1) != 0;

        const int w = 17 + static_cast<int>(rng() % 48);
        const int h = 17 + static_cast<int>(rng() % 48);
        auto field = Pipeline::brushWeightField(m, w, h);
        REQUIRE(field != nullptr);
        REQUIRE(field->size() == static_cast<size_t>(w) * h);

        // Compare every pixel against the sparse maskWeight API.
        for (int y = 0; y < h; ++y) {
            const float ny = (h > 1) ? static_cast<float>(y) / (h - 1) : 0.0f;
            for (int x = 0; x < w; ++x) {
                const float nx =
                    (w > 1) ? static_cast<float>(x) / (w - 1) : 0.0f;
                const float ref = Pipeline::maskWeight(m, w, h, nx, ny);
                const float got =
                    (*field)[static_cast<size_t>(y) * w + x];
                REQUIRE(got == Approx(ref).margin(1e-5f));
            }
        }
    }
}

TEST_CASE("MaskRaster: brushWeightField also rasterizes non-brush types",
          "[mask][raster]") {
    // Linear gradient: spot-check the field against maskWeight.
    Mask lin;
    lin.type = MaskType::LinearGradient;
    lin.linear.startX = 0.1f;
    lin.linear.startY = 0.0f;
    lin.linear.endX = 0.9f;
    lin.linear.endY = 1.0f;
    lin.linear.feather = 0.3f;
    const int w = 41, h = 37;
    auto linField = Pipeline::brushWeightField(lin, w, h);
    REQUIRE(linField != nullptr);
    REQUIRE(linField->size() == static_cast<size_t>(w) * h);
    for (int y = 0; y < h; y += 2)
        for (int x = 0; x < w; x += 2) {
            const float nx = (w > 1) ? static_cast<float>(x) / (w - 1) : 0.0f;
            const float ny = (h > 1) ? static_cast<float>(y) / (h - 1) : 0.0f;
            REQUIRE((*linField)[static_cast<size_t>(y) * w + x] ==
                    Approx(Pipeline::maskWeight(lin, w, h, nx, ny))
                        .margin(1e-5f));
        }

    // Radial gradient (with the radial invert flag folded in).
    Mask rad;
    rad.type = MaskType::RadialGradient;
    rad.radial.centerX = 0.5f;
    rad.radial.centerY = 0.5f;
    rad.radial.radiusX = 0.4f;
    rad.radial.radiusY = 0.3f;
    rad.radial.feather = 0.6f;
    rad.radial.invert = true;
    auto radField = Pipeline::brushWeightField(rad, w, h);
    REQUIRE(radField != nullptr);
    for (int y = 0; y < h; y += 2)
        for (int x = 0; x < w; x += 2) {
            const float nx = (w > 1) ? static_cast<float>(x) / (w - 1) : 0.0f;
            const float ny = (h > 1) ? static_cast<float>(y) / (h - 1) : 0.0f;
            REQUIRE((*radField)[static_cast<size_t>(y) * w + x] ==
                    Approx(Pipeline::maskWeight(rad, w, h, nx, ny))
                        .margin(1e-5f));
        }

    // None type: constant field, 0 by default and 1 when inverted.
    Mask none;
    none.type = MaskType::None;
    auto noneField = Pipeline::brushWeightField(none, w, h);
    REQUIRE(noneField != nullptr);
    REQUIRE(std::all_of(noneField->begin(), noneField->end(),
                        [](float v) { return v == 0.0f; }));
    none.invert = true;
    auto noneInv = Pipeline::brushWeightField(none, w, h);
    REQUIRE(std::all_of(noneInv->begin(), noneInv->end(),
                        [](float v) { return v == 1.0f; }));
}

TEST_CASE("MaskRaster: weight-field cache hits on same geometry, misses on change",
          "[mask][raster][cache]") {
    Mask m;
    m.type = MaskType::Brush;
    m.brush.push_back(BrushDab{0.4f, 0.4f, 0.2f, 0.6f});
    const int w = 64, h = 64;

    // Same (geometry, w, h) returns the same cached shared_ptr.
    auto a = Pipeline::brushWeightField(m, w, h);
    auto b = Pipeline::brushWeightField(m, w, h);
    REQUIRE(a != nullptr);
    REQUIRE(a.get() == b.get());

    // opacity is NOT part of the geometry hash -> still a hit.
    m.opacity = 0.5;
    auto c = Pipeline::brushWeightField(m, w, h);
    REQUIRE(a.get() == c.get());

    // changing the dab geometry -> miss.
    m.brush[0].x = 0.6f;
    auto d = Pipeline::brushWeightField(m, w, h);
    REQUIRE(a.get() != d.get());

    // changing invert -> miss (invert is part of the geometry).
    m.invert = true;
    auto e = Pipeline::brushWeightField(m, w, h);
    REQUIRE(d.get() != e.get());

    // adding a dab -> miss.
    m.invert = false;
    m.brush[0].x = 0.4f;
    m.brush.push_back(BrushDab{0.8f, 0.8f, 0.1f, 0.9f});
    auto f = Pipeline::brushWeightField(m, w, h);
    REQUIRE(d.get() != f.get());

    // local-adjustment fields are NOT geometry -> still a hit.
    m.exposure = 1.5;
    m.contrast = 40.0;
    auto g = Pipeline::brushWeightField(m, w, h);
    REQUIRE(f.get() == g.get());

    // different dimensions -> different cache key -> miss.
    auto hDim = Pipeline::brushWeightField(m, 128, 64);
    REQUIRE(f.get() != hDim.get());
}

TEST_CASE("MaskRaster: geometryHash covers only geometry + invert",
          "[mask][raster][hash]") {
    // Equal masks hash equally.
    Mask a;
    a.type = MaskType::Brush;
    a.brush.push_back(BrushDab{0.5f, 0.5f, 0.2f, 0.6f});
    Mask b = a;
    REQUIRE(a.geometryHash() == b.geometryHash());

    // opacity / local-adjustment fields do NOT change the geometry hash.
    b.opacity = 0.25;
    b.exposure = 1.5;
    b.contrast = -30.0;
    b.clarity = 20.0;
    b.saturation = 10.0;
    REQUIRE(a.geometryHash() == b.geometryHash());

    // dab x / radius / softness each change the hash.
    b.brush[0].x = 0.7f;
    REQUIRE(a.geometryHash() != b.geometryHash());
    b.brush[0].x = 0.5f;
    b.brush[0].radius = 0.4f;
    REQUIRE(a.geometryHash() != b.geometryHash());
    b.brush[0].radius = 0.2f;
    b.brush[0].softness = 0.9f;
    REQUIRE(a.geometryHash() != b.geometryHash());

    // adding / removing a dab changes the hash (dab sequence is geometry).
    b.brush[0].softness = 0.6f;
    b.brush.push_back(BrushDab{0.8f, 0.8f, 0.1f, 0.5f});
    REQUIRE(a.geometryHash() != b.geometryHash());
    b.brush.pop_back();
    REQUIRE(a.geometryHash() == b.geometryHash());

    // invert changes the hash.
    b.invert = true;
    REQUIRE(a.geometryHash() != b.geometryHash());

    // linear gradient geometry (end point / feather) changes the hash.
    Mask lin;
    lin.type = MaskType::LinearGradient;
    const size_t lin0 = lin.geometryHash();
    lin.linear.endY = 0.6f;
    REQUIRE(lin.geometryHash() != lin0);
    lin.linear.feather = 0.2f;
    REQUIRE(lin.geometryHash() != lin0);
    const size_t lin1 = lin.geometryHash();
    lin.opacity = 0.5;  // still not geometry
    REQUIRE(lin.geometryHash() == lin1);

    // radial gradient geometry (radius / radial invert) changes the hash.
    Mask rad;
    rad.type = MaskType::RadialGradient;
    const size_t rad0 = rad.geometryHash();
    rad.radial.radiusX = 0.6f;
    REQUIRE(rad.geometryHash() != rad0);
    rad.radial.invert = true;
    REQUIRE(rad.geometryHash() != rad0);
    const size_t rad1 = rad.geometryHash();
    rad.opacity = 0.9;  // still not geometry
    REQUIRE(rad.geometryHash() == rad1);
}

TEST_CASE("MaskRaster: 500-dab brush rasterizes quickly (no O(P*D) regression)",
          "[mask][raster][smoke]") {
    Mask m;
    m.type = MaskType::Brush;
    m.brush.reserve(500);
    std::mt19937 rng(9876);
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    for (int i = 0; i < 500; ++i) {
        BrushDab dab;
        dab.x = u01(rng);
        dab.y = u01(rng);
        dab.radius = 0.01f + 0.06f * u01(rng);
        dab.softness = u01(rng);
        m.brush.push_back(dab);
    }
    const int w = 1920, h = 1080;

    const auto t0 = std::chrono::steady_clock::now();
    auto field = Pipeline::brushWeightField(m, w, h);
    const auto t1 = std::chrono::steady_clock::now();

    REQUIRE(field != nullptr);
    REQUIRE(field->size() == static_cast<size_t>(w) * h);
    // At least one dab covers some pixel -> the field is not all zeros.
    REQUIRE(std::any_of(field->begin(), field->end(),
                        [](float v) { return v > 0.0f; }));

    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    INFO("500-dab rasterize took " << ms << " ms");
    REQUIRE(ms < 2000.0);
}

TEST_CASE("MaskRaster: applyLocalAdjustments matches a maskWeight reference",
          "[mask][raster]") {
    const int w = 64, h = 64;
    std::vector<float> src(static_cast<size_t>(w) * h * 3);
    std::mt19937 rng(2024);
    std::uniform_real_distribution<float> u01(0.02f, 0.98f);
    for (float& v : src)
        v = u01(rng);

    // Two brush masks: one plain, one inverted, with a spread of local
    // adjustments (no clarity -- its blurred-luma path is excluded from the
    // reference, and the pipeline keeps that path unchanged).
    Adjustments a;
    Mask m1;
    m1.type = MaskType::Brush;
    m1.brush.push_back(BrushDab{0.3f, 0.3f, 0.25f, 0.7f});
    m1.brush.push_back(BrushDab{0.7f, 0.6f, 0.3f, 0.4f});
    m1.opacity = 0.8;
    m1.exposure = 1.2;
    m1.contrast = 25.0;
    m1.highlights = 30.0;
    m1.shadows = -20.0;
    m1.whites = 15.0;
    m1.blacks = -10.0;
    m1.temperature = 8.0;
    m1.tint = -5.0;
    m1.dehaze = 10.0;
    m1.saturation = 15.0;
    a.masks.push_back(m1);

    Mask m2;
    m2.type = MaskType::Brush;
    m2.brush.push_back(BrushDab{0.5f, 0.5f, 0.4f, 0.5f});
    m2.invert = true;
    m2.opacity = 0.5;
    m2.exposure = -0.8;
    m2.saturation = -25.0;
    a.masks.push_back(m2);

    auto fieldPath = src;
    Pipeline::applyLocalAdjustments(a, fieldPath.data(), w, h);

    auto refPath = src;
    referenceApplyLocalAdjustments(a, refPath.data(), w, h);

    for (size_t i = 0; i < src.size(); ++i)
        REQUIRE(fieldPath[i] == Approx(refPath[i]).margin(1e-5f));
}
