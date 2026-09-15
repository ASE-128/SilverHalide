// PHASE_4 range-mask (colour range / luminance range sub-mask) tests.
//
// Covers:
//   - colourSubMaskWeight: per-pixel HSL-distance band against a sampled
//     reference (selected / excluded / disabled-identity).
//   - luminanceSubMaskWeight: per-pixel relative-luminance band (selected /
//     excluded / disabled-identity / smoothness widens the transition).
//   - combinedMaskWeight: geometry x colour x luminance intersection
//     (multiply) and union (max), plus the no-sub-mask regression that it
//     equals maskWeight exactly.
//   - applyLocalAdjustments: a geometry mask + colour sub-mask lifts only the
//     matched pixels locally.
//   - ProfileIO JSON round-trip of a Mask carrying every sub-mask field.
//   - hash correctness: every sub-mask field changes Mask::hash / geometryHash
//     and the incremental stage-E hash; local-adjustment values do not change
//     geometryHash.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

#include "engine/Adjustments.h"
#include "engine/ImageBuffer.h"
#include "engine/Pipeline.h"
#include "utils/ProfileIO.h"

using namespace Catch;
using namespace lrc;

TEST_CASE("RangeMasks: colour sub-mask weight selects the reference, excludes "
          "its opposite hue", "[mask][rangemask]") {
    Mask m;
    m.useColorRange = true;
    m.colorRefR = 1.0f;  // pure red reference.
    m.colorRefG = 0.0f;
    m.colorRefB = 0.0f;
    m.colorRange = 100.0f;      // maximum band.
    m.colorSmoothness = 0.0f;   // hard transition.

    // The reference colour itself is fully selected.
    REQUIRE(Pipeline::colorSubMaskWeight(m, 1.0f, 0.0f, 0.0f) ==
            Approx(1.0f).margin(0.01f));
    // Cyan is 180 degrees away in hue -> excluded.
    REQUIRE(Pipeline::colorSubMaskWeight(m, 0.0f, 1.0f, 1.0f) ==
            Approx(0.0f).margin(0.01f));
    // A neutral grey (zero saturation) does not match a saturated reference.
    REQUIRE(Pipeline::colorSubMaskWeight(m, 0.3f, 0.3f, 0.3f) ==
            Approx(0.0f).margin(0.01f));
}

TEST_CASE("RangeMasks: colour sub-mask band width controls selection",
          "[mask][rangemask]") {
    Mask m;
    m.useColorRange = true;
    m.colorRefR = 1.0f;
    m.colorRefG = 0.0f;
    m.colorRefB = 0.0f;
    m.colorRange = 40.0f;       // moderate band.
    m.colorSmoothness = 0.0f;

    // A moderately reddish pixel sits inside the 40% band.
    REQUIRE(Pipeline::colorSubMaskWeight(m, 0.7f, 0.2f, 0.2f) > 0.9f);
    // A blue pixel is far enough in hue to be excluded.
    REQUIRE(Pipeline::colorSubMaskWeight(m, 0.2f, 0.2f, 0.7f) < 0.01f);

    // Smoothness softens the band edge: the transition begins earlier and is
    // more gradual, so a mid-band pixel keeps a partial weight with the hard
    // step but a much smaller one with the soft transition.
    Mask soft = m;
    soft.colorSmoothness = 100.0f;
    const float hard = Pipeline::colorSubMaskWeight(m, 0.79f, 0.21f, 0.21f);
    const float softW = Pipeline::colorSubMaskWeight(soft, 0.79f, 0.21f, 0.21f);
    REQUIRE(hard > 0.1f);                          // mid-band partial weight
    REQUIRE(softW != Approx(hard).margin(0.1f));   // smoothness changes the falloff
    REQUIRE(softW < hard);                         // softer edge decays earlier
}

TEST_CASE("RangeMasks: disabled colour sub-mask is the product identity",
          "[mask][rangemask]") {
    Mask m;
    m.useColorRange = false;
    // Returns 1.0 regardless of the pixel colour.
    REQUIRE(Pipeline::colorSubMaskWeight(m, 1.0f, 0.0f, 0.0f) ==
            Approx(1.0f).margin(1e-6f));
    REQUIRE(Pipeline::colorSubMaskWeight(m, 0.0f, 0.0f, 0.0f) ==
            Approx(1.0f).margin(1e-6f));
}

TEST_CASE("RangeMasks: luminance sub-mask weight selects around the reference",
          "[mask][rangemask]") {
    Mask m;
    m.useLuminance = true;
    m.luminanceRef = 50.0f;     // 50% relative luminance.
    m.luminanceRange = 50.0f;
    m.luminanceSmoothness = 0.0f;

    // A mid-grey pixel has L == 0.5 -> fully selected.
    REQUIRE(Pipeline::luminanceSubMaskWeight(m, 0.5f, 0.5f, 0.5f) ==
            Approx(1.0f).margin(0.01f));

    // A narrow band excludes a much brighter pixel.
    m.luminanceRange = 10.0f;
    REQUIRE(Pipeline::luminanceSubMaskWeight(m, 0.9f, 0.9f, 0.9f) ==
            Approx(0.0f).margin(0.01f));
    // The mid-grey pixel is still selected by the narrow band.
    REQUIRE(Pipeline::luminanceSubMaskWeight(m, 0.5f, 0.5f, 0.5f) ==
            Approx(1.0f).margin(0.01f));
}

TEST_CASE("RangeMasks: disabled luminance sub-mask is the product identity",
          "[mask][rangemask]") {
    Mask m;
    m.useLuminance = false;
    REQUIRE(Pipeline::luminanceSubMaskWeight(m, 0.0f, 0.0f, 0.0f) ==
            Approx(1.0f).margin(1e-6f));
    REQUIRE(Pipeline::luminanceSubMaskWeight(m, 1.0f, 1.0f, 1.0f) ==
            Approx(1.0f).margin(1e-6f));
}

namespace {

// A radial gradient mask used by the combination tests.
Mask makeRadialMask() {
    Mask m;
    m.type = MaskType::RadialGradient;
    m.radial.centerX = 0.5f;
    m.radial.centerY = 0.5f;
    m.radial.radiusX = 0.3f;
    m.radial.radiusY = 0.3f;
    m.radial.feather = 0.0f;
    return m;
}

} // namespace

TEST_CASE("RangeMasks: combined weight equals maskWeight when no sub-mask",
          "[mask][rangemask]") {
    Mask m = makeRadialMask();
    const int w = 64, h = 64;
    // No sub-mask: the combined weight is bit-for-bit the geometry weight.
    REQUIRE(Pipeline::combinedMaskWeight(m, w, h, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f) ==
            Approx(Pipeline::maskWeight(m, w, h, 0.5f, 0.5f)).margin(1e-5f));
    REQUIRE(Pipeline::combinedMaskWeight(m, w, h, 0.0f, 0.0f, 0.2f, 0.2f, 0.7f) ==
            Approx(Pipeline::maskWeight(m, w, h, 0.0f, 0.0f)).margin(1e-5f));
}

TEST_CASE("RangeMasks: intersection (multiply) and union (max) combination",
          "[mask][rangemask]") {
    Mask m = makeRadialMask();
    const int w = 64, h = 64;
    m.useColorRange = true;
    m.colorRefR = 1.0f;  // pure red reference.
    m.colorRefG = 0.0f;
    m.colorRefB = 0.0f;
    m.colorRange = 40.0f;
    m.colorSmoothness = 0.0f;
    m.maskUnion = false;  // default intersection.

    // Centre is inside the ellipse (geo 1). Red pixel: 1 * 1 = 1.
    REQUIRE(Pipeline::combinedMaskWeight(m, w, h, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f) ==
            Approx(1.0f).margin(0.01f));
    // Centre, blue pixel: 1 * 0 = 0.
    REQUIRE(Pipeline::combinedMaskWeight(m, w, h, 0.5f, 0.5f, 0.2f, 0.2f, 0.7f) ==
            Approx(0.0f).margin(0.01f));
    // Corner is outside the ellipse (geo 0), red pixel: 0 * 1 = 0.
    REQUIRE(Pipeline::combinedMaskWeight(m, w, h, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f) ==
            Approx(0.0f).margin(0.01f));

    // Union mode: max(geo, colour).
    m.maskUnion = true;
    REQUIRE(Pipeline::combinedMaskWeight(m, w, h, 0.5f, 0.5f, 0.2f, 0.2f, 0.7f) ==
            Approx(1.0f).margin(0.01f));  // centre geo 1 dominates.
    REQUIRE(Pipeline::combinedMaskWeight(m, w, h, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f) ==
            Approx(1.0f).margin(0.01f));  // corner red colour dominates.
}

TEST_CASE("RangeMasks: three-way intersection (geometry x colour x luminance)",
          "[mask][rangemask]") {
    Mask m = makeRadialMask();
    const int w = 64, h = 64;
    m.useColorRange = true;
    m.colorRefR = 1.0f;
    m.colorRefG = 0.0f;
    m.colorRefB = 0.0f;
    m.colorRange = 40.0f;
    m.colorSmoothness = 0.0f;
    m.useLuminance = true;
    m.luminanceRef = 50.0f;
    m.luminanceRange = 10.0f;  // narrow: only pixels near L=0.5 pass.
    m.luminanceSmoothness = 0.0f;
    m.maskUnion = false;

    // A warm orange pixel at the ellipse centre matches geometry, colour AND
    // the narrow luminance band (L ~ 0.5) -> 1 * 1 * 1 = 1.
    REQUIRE(Pipeline::combinedMaskWeight(m, w, h, 0.5f, 0.5f, 1.0f, 0.4f, 0.0f) ==
            Approx(1.0f).margin(0.01f));

    // A bright red pixel matches geometry + colour but fails the luminance band
    // (L ~ 0.23 far from 0.5) -> 1 * 1 * 0 = 0.
    REQUIRE(Pipeline::combinedMaskWeight(m, w, h, 0.5f, 0.5f, 0.9f, 0.05f, 0.05f) ==
            Approx(0.0f).margin(0.01f));

    // A blue pixel fails colour -> 0 even though geometry/luminance pass.
    REQUIRE(Pipeline::combinedMaskWeight(m, w, h, 0.5f, 0.5f, 0.2f, 0.2f, 0.7f) ==
            Approx(0.0f).margin(0.01f));
}

TEST_CASE("RangeMasks: local adjustments apply through the colour sub-mask",
          "[mask][rangemask]") {
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
    m.type = MaskType::LinearGradient;  // full-field geometry.
    m.linear.startX = 0.0f;
    m.linear.startY = 0.0f;
    m.linear.endX = 1.0f;
    m.linear.endY = 0.0f;
    m.linear.feather = 0.0f;
    m.useColorRange = true;
    m.colorRefR = 1.0f;
    m.colorRefG = 0.0f;
    m.colorRefB = 0.0f;
    m.colorRange = 40.0f;
    m.colorSmoothness = 0.0f;
    m.exposure = 1.0;  // +1 stop.
    a.masks.push_back(m);

    Pipeline::applyLocalAdjustments(a, data, w, h);

    // 红区像素被选中: 红通道约 2 倍变亮 (no clamp inside applyLocalAdjustments).
    REQUIRE(data[0] == Approx(1.4f).margin(0.1f));
    // 蓝区像素被排除: 保持不变, 且明显暗于红区.
    REQUIRE(data[static_cast<size_t>(w - 1) * 3] == Approx(0.2f).margin(0.01f));
    REQUIRE(data[0] > data[static_cast<size_t>(w - 1) * 3]);
}

TEST_CASE("RangeMasks: Mask JSON round-trip is lossless with every sub-mask "
          "field", "[mask][rangemask][profile]") {
    Adjustments a;
    Mask m;
    m.type = MaskType::RadialGradient;
    m.radial.centerX = 0.6f;
    m.radial.centerY = 0.4f;
    m.radial.radiusX = 0.3f;
    m.radial.radiusY = 0.2f;
    m.radial.feather = 0.7f;
    m.useColorRange = true;
    m.colorRefR = 0.9f;
    m.colorRefG = 0.1f;
    m.colorRefB = 0.1f;
    m.colorRange = 35.0f;
    m.colorSmoothness = 60.0f;
    m.useLuminance = true;
    m.luminanceRef = 62.0f;
    m.luminanceRange = 25.0f;
    m.luminanceSmoothness = 40.0f;
    m.maskUnion = true;
    m.opacity = 0.8;
    m.exposure = 1.2;
    a.masks.push_back(m);

    const auto json = ProfileIO::toJson(a);
    const Adjustments b = ProfileIO::fromJson(json);
    REQUIRE(a == b);
    REQUIRE(a.hash() == b.hash());
}

TEST_CASE("RangeMasks: sub-mask fields drive the mask hash and stage-E hash",
          "[mask][rangemask][hash]") {
    Mask a;
    a.type = MaskType::Brush;
    a.brush.push_back(BrushDab{0.5f, 0.5f, 0.2f, 0.6f});
    Mask b = a;

    REQUIRE(a.hash() == b.hash());
    REQUIRE(a.geometryHash() == b.geometryHash());

    // Enabling the colour sub-mask + reference changes both hashes.
    b.useColorRange = true;
    b.colorRefR = 0.9f;
    b.colorRefG = 0.1f;
    b.colorRefB = 0.1f;
    b.colorRange = 40.0f;
    b.colorSmoothness = 50.0f;
    REQUIRE(a.hash() != b.hash());
    REQUIRE(a.geometryHash() != b.geometryHash());

    // Luminance fields change the hash too.
    Mask c = b;
    c.useLuminance = true;
    c.luminanceRef = 60.0f;
    c.luminanceRange = 30.0f;
    c.luminanceSmoothness = 20.0f;
    REQUIRE(b.hash() != c.hash());
    REQUIRE(b.geometryHash() != c.geometryHash());

    // Union mode participates in the hash.
    Mask d = c;
    d.maskUnion = true;
    REQUIRE(c.hash() != d.hash());
    REQUIRE(c.geometryHash() != d.geometryHash());

    // Local-adjustment values change hash() but NOT geometryHash (the weight
    // field is unchanged).
    Mask e = d;
    e.exposure = 1.5;
    REQUIRE(e.hash() != d.hash());
    REQUIRE(e.geometryHash() == d.geometryHash());

    // The incremental stage-E hash (spatial / masks) follows the mask hash.
    Adjustments adj;
    adj.masks.push_back(d);
    const size_t stageE0 = adj.hashStageE();
    adj.masks[0].colorRange = 55.0f;
    REQUIRE(adj.hashStageE() != stageE0);
}
