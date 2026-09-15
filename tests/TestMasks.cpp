#include <algorithm>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/Adjustments.h"
#include "engine/ImageBuffer.h"
#include "engine/Pipeline.h"
#include "utils/ProfileIO.h"

using namespace Catch;
using namespace lrc;

TEST_CASE("Mask: linear gradient weight is 1 on the axis and 0 past the edges",
          "[mask]") {
    const int w = 101, h = 101;
    Mask m;
    m.type = MaskType::LinearGradient;
    m.linear.startX = 0.0f;
    m.linear.startY = 0.0f;
    m.linear.endX = 1.0f;
    m.linear.endY = 0.0f;
    m.linear.feather = 0.0f;

    // Anywhere along the axis between the edges is fully masked (the strip is
    // infinite in the perpendicular direction).
    REQUIRE(Pipeline::maskWeight(m, w, h, 0.5f, 0.5f) == Approx(1.0f).margin(0.01f));
    REQUIRE(Pipeline::maskWeight(m, w, h, 0.5f, 0.0f) == Approx(1.0f).margin(0.01f));
    // Past either edge the weight drops to zero.
    REQUIRE(Pipeline::maskWeight(m, w, h, -1.0f, 0.5f) == Approx(0.0f).margin(0.01f));
    REQUIRE(Pipeline::maskWeight(m, w, h, 2.0f, 0.5f) == Approx(0.0f).margin(0.01f));

    // Inverting flips the strip inside-out.
    m.invert = true;
    REQUIRE(Pipeline::maskWeight(m, w, h, 0.5f, 0.5f) == Approx(0.0f).margin(0.01f));
    REQUIRE(Pipeline::maskWeight(m, w, h, -1.0f, 0.5f) == Approx(1.0f).margin(0.01f));
}

TEST_CASE("Mask: radial gradient weight is 1 at center and 0 at the corner",
          "[mask]") {
    const int w = 101, h = 101;
    Mask m;
    m.type = MaskType::RadialGradient;
    m.radial.centerX = 0.5f;
    m.radial.centerY = 0.5f;
    m.radial.radiusX = 0.5f;
    m.radial.radiusY = 0.5f;
    m.radial.feather = 0.0f;

    REQUIRE(Pipeline::maskWeight(m, w, h, 0.5f, 0.5f) == Approx(1.0f).margin(0.01f));
    // Corner is at normalized distance sqrt(2) > 1 from the center.
    REQUIRE(Pipeline::maskWeight(m, w, h, 0.0f, 0.0f) == Approx(0.0f).margin(0.01f));

    m.invert = true;
    REQUIRE(Pipeline::maskWeight(m, w, h, 0.0f, 0.0f) == Approx(1.0f).margin(0.01f));
}

TEST_CASE("Mask: brush weight is 1 at the dab center and 0 far away",
          "[mask]") {
    const int w = 101, h = 101;
    Mask m;
    m.type = MaskType::Brush;
    m.brush.push_back(BrushDab{0.5f, 0.5f, 0.2f, 0.0f});
    REQUIRE(Pipeline::maskWeight(m, w, h, 0.5f, 0.5f) == Approx(1.0f).margin(0.01f));
    REQUIRE(Pipeline::maskWeight(m, w, h, 0.0f, 0.0f) < 0.01f);
}

TEST_CASE("Mask: local exposure lifts the fully-masked image by 4x",
          "[mask]") {
    const int w = 8, h = 8;
    std::vector<float> px(static_cast<size_t>(w) * h * 3, 0.2f);
    Adjustments a;
    Mask m;
    m.type = MaskType::LinearGradient;
    m.linear.startX = 0.0f;
    m.linear.startY = 0.0f;
    m.linear.endX = 1.0f;
    m.linear.endY = 0.0f;
    m.linear.feather = 0.0f;
    m.exposure = 2.0; // +2 stops = 4x
    a.masks.push_back(m);

    Pipeline::applySpatialAdjustments(a, px.data(), w, h);
    REQUIRE(px[0] == Approx(0.8f).margin(0.05f));
    REQUIRE(px[3] == Approx(0.8f).margin(0.05f));
}

TEST_CASE("Mask: radial local exposure only affects the ellipse", "[mask]") {
    const int w = 64, h = 64;
    std::vector<float> px(static_cast<size_t>(w) * h * 3, 0.2f);
    Adjustments a;
    Mask m;
    m.type = MaskType::RadialGradient;
    m.radial.centerX = 0.5f;
    m.radial.centerY = 0.5f;
    m.radial.radiusX = 0.25f;
    m.radial.radiusY = 0.25f;
    m.radial.feather = 0.0f;
    m.exposure = 2.0;
    a.masks.push_back(m);

    Pipeline::applySpatialAdjustments(a, px.data(), w, h);
    // Center pixel sits inside the ellipse -> 4x lift.
    REQUIRE(px[(32 * 64 + 32) * 3] == Approx(0.8f).margin(0.05f));
    // Corner is far outside -> untouched.
    REQUIRE(px[0] == Approx(0.2f).margin(0.01f));
}

TEST_CASE("Mask: opacity scales the effect", "[mask]") {
    const int w = 8, h = 8;
    std::vector<float> px(static_cast<size_t>(w) * h * 3, 0.2f);
    Adjustments a;
    Mask m;
    m.type = MaskType::LinearGradient;
    m.linear.startX = 0.0f;
    m.linear.startY = 0.0f;
    m.linear.endX = 1.0f;
    m.linear.endY = 0.0f;
    m.linear.feather = 0.0f;
    m.exposure = 2.0;
    m.opacity = 0.5; // half effect: 0.2 -> 0.2 + 0.5*(0.8-0.2) = 0.5
    a.masks.push_back(m);

    Pipeline::applySpatialAdjustments(a, px.data(), w, h);
    REQUIRE(px[0] == Approx(0.5f).margin(0.05f));
}

TEST_CASE("Mask: masks with no effect are ignored", "[mask]") {
    const int w = 8, h = 8;
    std::vector<float> px(static_cast<size_t>(w) * h * 3, 0.2f);
    std::vector<float> expected = px;
    Adjustments a;
    Mask m;
    m.type = MaskType::LinearGradient;
    m.linear.startX = 0.0f;
    m.linear.endX = 1.0f;
    // All adjustment fields zero -> nothing applied.
    a.masks.push_back(m);
    Pipeline::applySpatialAdjustments(a, px.data(), w, h);
    REQUIRE(px == expected);
}

TEST_CASE("ColorTarget: hue shift only moves the matched colour", "[colortarget]") {
    std::vector<float> px{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f}; // red, blue
    Adjustments a;
    ColorTarget ct;
    ct.enabled = true;
    ct.refR = 1.0f; // reference red
    ct.refG = 0.0f;
    ct.refB = 0.0f;
    ct.hueShift = 30.0;
    ct.range = 20.0;
    ct.smoothness = 50.0;
    ct.amount = 100.0;
    a.colorTargets.push_back(ct);

    Pipeline::applyAdjustments(a, px.data(), 2);
    // Red shifted toward yellow: green rises strongly.
    REQUIRE(px[1] > 0.4f);
    // Blue is 120°+ away in hue and gets almost no weight.
    REQUIRE(px[3] < 0.3f);
    REQUIRE(px[5] > 0.6f);
}

TEST_CASE("ColorTarget: saturation boost increases channel separation",
          "[colortarget]") {
    std::vector<float> px{0.6f, 0.25f, 0.15f};
    Adjustments a;
    ColorTarget ct;
    ct.enabled = true;
    ct.refR = 0.6f;
    ct.refG = 0.25f;
    ct.refB = 0.15f;
    ct.saturation = 100.0;
    ct.range = 30.0;
    ct.smoothness = 50.0;
    ct.amount = 100.0;
    a.colorTargets.push_back(ct);

    const float before = std::max({px[0], px[1], px[2]}) -
                         std::min({px[0], px[1], px[2]});
    Pipeline::applyAdjustments(a, px.data(), 1);
    const float after = std::max({px[0], px[1], px[2]}) -
                        std::min({px[0], px[1], px[2]});
    REQUIRE(after > before);
}

TEST_CASE("ColorTarget: disabled target is a no-op", "[colortarget]") {
    std::vector<float> px{0.5f, 0.5f, 0.5f};
    const std::vector<float> expected = px;
    Adjustments a;
    ColorTarget ct;
    ct.enabled = false; // off
    ct.refR = 0.5f;
    ct.refG = 0.5f;
    ct.refB = 0.5f;
    ct.saturation = 100.0;
    a.colorTargets.push_back(ct);
    Pipeline::applyAdjustments(a, px.data(), 1);
    REQUIRE(px == expected);
}

TEST_CASE("Masks and color targets round-trip through ProfileIO JSON",
          "[mask][colortarget][profile]") {
    Adjustments a;
    Mask m;
    m.type = MaskType::RadialGradient;
    m.radial.centerX = 0.6f;
    m.radial.centerY = 0.4f;
    m.radial.radiusX = 0.3f;
    m.radial.radiusY = 0.2f;
    m.radial.feather = 0.7f;
    m.radial.invert = true;
    m.opacity = 0.8;
    m.exposure = 1.2;
    m.clarity = 35.0;
    m.saturation = -20.0;
    a.masks.push_back(m);

    Mask brush;
    brush.type = MaskType::Brush;
    brush.brush.push_back(BrushDab{0.5f, 0.5f, 0.15f, 0.6f});
    brush.brush.push_back(BrushDab{0.7f, 0.5f, 0.2f, 0.8f});
    brush.opacity = 0.9;
    brush.highlights = -40.0;
    a.masks.push_back(brush);

    ColorTarget ct;
    ct.enabled = true;
    ct.refR = 0.9f;
    ct.refG = 0.3f;
    ct.refB = 0.1f;
    ct.hueShift = 15.0;
    ct.range = 40.0;
    ct.smoothness = 60.0;
    ct.amount = 80.0;
    a.colorTargets.push_back(ct);

    const auto json = ProfileIO::toJson(a);
    const Adjustments b = ProfileIO::fromJson(json);
    REQUIRE(a == b);
    REQUIRE(a.hash() == b.hash());
}

TEST_CASE("AiMask: full parameter round-trip through ProfileIO JSON",
          "[mask][ai][profile][ai_masks]") {
    // R-12 regression: an AI mask (type AiMask / value 5) must survive a
    // toJson->fromJson round-trip unchanged, including every non-default
    // parameter. Before the fix the type was clamped to ColorRange and the
    // AI params were silently dropped.
    Adjustments a;
    Mask m;
    m.type = MaskType::AiMask;
    m.ai.target = AiMaskTarget::Sky;
    m.ai.threshold = 0.7f;
    m.ai.feather = 0.2f;
    m.ai.invert = true;
    m.ai.modelInputSize = 640;
    m.ai.cachedWeightVersion = 3;
    m.ai.modelIdOverride = "builtin://sky-override";
    m.opacity = 0.85;
    m.exposure = 0.4;
    a.masks.push_back(m);

    // An unrelated mask alongside must be left untouched by the aiMasks pass.
    Mask grad;
    grad.type = MaskType::LinearGradient;
    grad.linear.startX = 0.1f;
    grad.linear.startY = 0.2f;
    grad.linear.endX = 0.9f;
    grad.linear.endY = 0.8f;
    grad.linear.feather = 0.3f;
    a.masks.push_back(grad);

    const auto json = ProfileIO::toJson(a);
    const Adjustments b = ProfileIO::fromJson(json);
    REQUIRE(a == b);
    REQUIRE(a.hash() == b.hash());

    // Explicitly confirm the persisted type is still AiMask (not downgraded).
    REQUIRE(b.masks[0].type == MaskType::AiMask);
    REQUIRE(b.masks[0].ai.target == AiMaskTarget::Sky);
    REQUIRE(b.masks[0].ai.modelIdOverride == "builtin://sky-override");
    REQUIRE(b.masks[1].type == MaskType::LinearGradient);
}

TEST_CASE("ColorRange: reference colour scores ~1, inverse colour ~0",
          "[mask][colorrange]") {
    ColorRangeMask m;
    m.hasReference = true;
    m.refR = 1.0f;  // 纯红参考 (pure red reference).
    m.refG = 0.0f;
    m.refB = 0.0f;

    // The sampled colour itself is fully selected.
    REQUIRE(Pipeline::colorRangeWeight(m, 1.0f, 0.0f, 0.0f) ==
            Approx(1.0f).margin(0.01f));
    // 纯青与红色相相差 180 度, 被色相带完全排除.
    REQUIRE(Pipeline::colorRangeWeight(m, 0.0f, 1.0f, 1.0f) ==
            Approx(0.0f).margin(0.01f));
}

TEST_CASE("ColorRange: a neutral grey is gated out", "[mask][colorrange]") {
    ColorRangeMask m;
    m.hasReference = true;
    m.refR = 1.0f;  // 纯红参考 (pure red reference).
    m.refG = 0.0f;
    m.refB = 0.0f;

    // 零饱和度触发灰色闸门, 权重归零.
    REQUIRE(Pipeline::colorRangeWeight(m, 0.18f, 0.18f, 0.18f) ==
            Approx(0.0f).margin(0.01f));
}

TEST_CASE("ColorRange: wider hue tolerance admits an off-hue reddish pixel",
          "[mask][colorrange]") {
    ColorRangeMask narrow;
    narrow.hasReference = true;
    narrow.refR = 1.0f;
    narrow.refG = 0.0f;
    narrow.refB = 0.0f;
    narrow.hueTolerance = 20.0;

    ColorRangeMask wide = narrow;
    wide.hueTolerance = 90.0;

    // 偏红但不纯: 色相距参考约 26 度, 处于窄带的模糊区外、宽带内.
    // (The spec's (0.8,0.2,0.2) has hue exactly 0 - the same as pure red - so
    // the two tolerances would score identically; this pixel separates them.)
    const float wNarrow = Pipeline::colorRangeWeight(narrow, 0.8f, 0.4f, 0.1f);
    const float wWide = Pipeline::colorRangeWeight(wide, 0.8f, 0.4f, 0.1f);
    REQUIRE(wWide > wNarrow);
}

TEST_CASE("ColorRange: local exposure lifts only the matched colour",
          "[mask][colorrange]") {
    const int w = 8, h = 8;
    auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
    float* data = reinterpret_cast<float*>(buf->data().data());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 3;
            if (x < w / 2) {  // 左半偏红.
                data[i] = 0.7f;
                data[i + 1] = 0.2f;
                data[i + 2] = 0.2f;
            } else {  // 右半偏蓝.
                data[i] = 0.2f;
                data[i + 1] = 0.2f;
                data[i + 2] = 0.7f;
            }
        }

    Adjustments a;
    Mask m;
    m.type = MaskType::ColorRange;
    m.color.hasReference = true;
    m.color.refR = 0.9f;
    m.color.refG = 0.1f;
    m.color.refB = 0.1f;
    m.color.hueTolerance = 40.0;
    m.color.satTolerance = 0.5;
    m.color.lumaTolerance = 0.5;
    m.color.softness = 0.5;
    m.exposure = 1.0;  // +1 stop.
    a.masks.push_back(m);

    Pipeline::applyLocalAdjustments(a, data, w, h);

    // 红区像素被完全选中: 红通道约 2 倍变亮.
    REQUIRE(data[0] == Approx(1.4f).margin(0.1f));
    // 蓝区像素几乎不变, 且明显暗于红区.
    REQUIRE(data[static_cast<size_t>(w - 1) * 3] == Approx(0.2f).margin(0.01f));
    REQUIRE(data[0] > data[static_cast<size_t>(w - 1) * 3]);
}

TEST_CASE("ColorRange: full-field ProfileIO JSON round-trip",
          "[mask][colorrange][profile]") {
    Adjustments a;
    Mask m;
    m.type = MaskType::ColorRange;
    m.color.hasReference = true;
    m.color.refR = 0.87f;
    m.color.refG = 0.33f;
    m.color.refB = 0.12f;
    m.color.hueTolerance = 65.0;
    m.color.satTolerance = 0.3;
    m.color.lumaTolerance = 0.7;
    m.color.softness = 0.4;
    m.opacity = 0.9;
    m.exposure = 1.3;
    m.contrast = 15.0;
    m.saturation = -20.0;
    a.masks.push_back(m);

    const auto json = ProfileIO::toJson(a);
    const Adjustments b = ProfileIO::fromJson(json);
    REQUIRE(a == b);
    REQUIRE(a.hash() == b.hash());
}

TEST_CASE("ColorRange: fromJson clamps an out-of-range type to AiMask",
          "[mask][colorrange][ai][profile]") {
    json::Value root = json::Value::object();
    json::Value masks = json::Value::array();
    json::Value mo = json::Value::object();
    // PHASE_13 / R-12: an out-of-range type clamps to the new ceiling
    // AiMask, never to ColorRange, so real AiMasks survive a round-trip.
    mo["type"] = 999;
    mo["hasReference"] = true;
    mo["refR"] = 0.6;
    mo["refG"] = 0.3;
    mo["refB"] = 0.1;
    mo["hueTolerance"] = 70.0;
    masks.push_back(mo);
    root["masks"] = std::move(masks);

    const Adjustments a = ProfileIO::fromJson(root);
    REQUIRE(a.masks.size() == 1);
    REQUIRE(a.masks[0].type == MaskType::AiMask);
}

TEST_CASE("ColorRange: geometryHash covers the reference but not exposure",
          "[mask][colorrange][hash]") {
    Mask a;
    a.type = MaskType::ColorRange;
    a.color.hasReference = true;
    a.color.refR = 0.9f;
    a.color.refG = 0.1f;
    a.color.refB = 0.1f;

    // 参考色属于几何 (it shapes the weight field).
    Mask b = a;
    b.color.refG = 0.3f;
    REQUIRE(a.geometryHash() != b.geometryHash());

    // 局部调整值不属于几何 (exposure excluded), 与其它蒙版类型一致.
    b = a;
    b.exposure = 1.5;
    REQUIRE(a.geometryHash() == b.geometryHash());
}
