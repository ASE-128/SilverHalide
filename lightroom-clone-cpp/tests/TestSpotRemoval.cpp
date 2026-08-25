// PHASE_5 spot removal / healing brush engine tests.
//
// Covers:
//   - Clone: target-circle pixels are replaced with the corresponding source
//     pixels (bilinear source sampling, soft feathered edge).
//   - Heal: the target region changes, the rim brightness is matched (the
//     dust is removed and the seam is continuous), and no NaN/inf appears.
//   - Out-of-bounds sources: a spot whose source hangs off the image edge
//     leaves every mapped target pixel untouched.
//   - Neutral fast path: an empty (or all-invisible) spotRemovals vector is a
//     bit-exact no-op.
//   - Normalized-coordinate consistency: the same normalized spot lands at the
//     same relative position on differently sized buffers.
//   - Hash sensitivity: spotRemovals participates in Adjustments::hash and
//     hashStageE, so a spot change invalidates the render caches.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/Adjustments.h"
#include "engine/Pipeline.h"

using namespace Catch;
using namespace lrc;

namespace {

// Reads the RGB triple of pixel (x, y) of a w-wide RGB-interleaved buffer.
std::array<float, 3> rgbAt(const std::vector<float>& px, int w, int x, int y) {
    const size_t i = (static_cast<size_t>(y) * w + x) * 3;
    return {px[i], px[i + 1], px[i + 2]};
}

}  // namespace

TEST_CASE("SpotRemoval: Clone copies the source circle onto the target",
          "[spotremoval]") {
    const int w = 64, h = 64;
    std::vector<float> px(static_cast<size_t>(w) * h * 3, 0.0f);
    auto set = [&](int x, int y, float r, float g, float b) {
        float* p = px.data() + (static_cast<size_t>(y) * w + x) * 3;
        p[0] = r;
        p[1] = g;
        p[2] = b;
    };

    // Fill blue, then draw a solid red source disk centred at normalized
    // (0.2, 0.2). The spot radius is 0.15 -> 9.6 px; the disk is drawn with a
    // 12 px radius so every mapped source sample inside the inner 80% band
    // lands well within the red disk.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            set(x, y, 0.0f, 0.0f, 1.0f);
    const float sxc = 0.2f * w, syc = 0.2f * h;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float dx = static_cast<float>(x) - sxc;
            const float dy = static_cast<float>(y) - syc;
            if (dx * dx + dy * dy <= 12.0f * 12.0f)
                set(x, y, 1.0f, 0.0f, 0.0f);
        }
    }

    Adjustments a;
    SpotRemoval s;
    s.type = SpotType::Clone;
    s.sourceX = 0.2f;
    s.sourceY = 0.2f;
    s.targetX = 0.7f;
    s.targetY = 0.7f;
    s.radius = 0.15f;
    s.visible = true;
    a.spotRemovals.push_back(s);

    Pipeline::applySpotRemoval(a, px.data(), w, h);

    // Pixel (44, 44) is ~1 px from the target centre (0.7*64 = 44.8) and maps
    // to the source centre, so it must be fully red.
    {
        const auto c = rgbAt(px, w, 44, 44);
        REQUIRE(c[0] == Approx(1.0f).margin(1e-3f));
        REQUIRE(c[1] == Approx(0.0f).margin(1e-3f));
        REQUIRE(c[2] == Approx(0.0f).margin(1e-3f));
    }

    // A pixel well outside the target circle (d ~ 11.2 > r = 9.6) stays blue.
    {
        const auto c = rgbAt(px, w, 44, 56);
        REQUIRE(c[2] == Approx(1.0f).margin(1e-3f));
        REQUIRE(c[0] < 0.01f);
    }

    // A pixel inside the feathered annulus (0.8r < d < r) is a soft blend
    // between the red source and the blue base: strictly between the two.
    {
        const auto c = rgbAt(px, w, 45, 54);  // d ~ 9.2
        REQUIRE(c[0] > 0.01f);
        REQUIRE(c[0] < 0.99f);
        REQUIRE(c[2] > 0.01f);
    }

    // The original source disk is untouched by the clone.
    {
        const auto c = rgbAt(px, w, 13, 13);
        REQUIRE(c[0] == Approx(1.0f).margin(1e-3f));
    }
}

TEST_CASE("SpotRemoval: Heal removes the dust and matches the rim brightness",
          "[spotremoval]") {
    const int w = 64, h = 64;
    std::vector<float> px(static_cast<size_t>(w) * h * 3, 0.0f);
    // Grey vertical gradient: luma 0.2 (top) .. 0.8 (bottom), uniform in x so
    // any horizontal step after healing is a seam artifact.
    for (int y = 0; y < h; ++y) {
        const float v = 0.2f + 0.6f * (static_cast<float>(y) / (h - 1));
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                px[(static_cast<size_t>(y) * w + x) * 3 + c] = v;
    }
    // A bright dust speck at the target centre (0.7, 0.7).
    const float txc = 0.7f * w, tyc = 0.7f * h;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float dx = static_cast<float>(x) - txc;
            const float dy = static_cast<float>(y) - tyc;
            if (dx * dx + dy * dy <= 3.0f * 3.0f)
                for (int c = 0; c < 3; ++c)
                    px[(static_cast<size_t>(y) * w + x) * 3 + c] = 1.0f;
        }
    }

    Adjustments a;
    SpotRemoval s;
    s.type = SpotType::Heal;
    s.sourceX = 0.2f;
    s.sourceY = 0.2f;
    s.targetX = 0.7f;
    s.targetY = 0.7f;
    s.radius = 0.15f;
    a.spotRemovals.push_back(s);

    Pipeline::applySpotRemoval(a, px.data(), w, h);

    // The target centre is no longer the bright dust (1.0). The healed value
    // sits near the local gradient value (~0.61 at y = 44.8) because the source
    // texture is scaled to match the target rim brightness.
    const float centre = px[(static_cast<size_t>(44) * w + 44) * 3];
    REQUIRE(centre > 0.4f);
    REQUIRE(centre < 0.9f);
    REQUIRE(std::abs(centre - 0.627f) < 0.15f);

    // Seam continuity: along the horizontal line through the target centre the
    // healed band has no hard step (the source gradient is constant in x).
    float maxStep = 0.0f;
    for (int x = 1; x < w; ++x) {
        const float lhs = px[(static_cast<size_t>(44) * w + x) * 3];
        const float rhs = px[(static_cast<size_t>(44) * w + x - 1) * 3];
        maxStep = std::max(maxStep, std::abs(lhs - rhs));
    }
    REQUIRE(maxStep < 0.1f);

    // No NaN/inf and everything stays normalized to [0,1].
    for (float v : px) {
        REQUIRE(std::isfinite(v));
        REQUIRE(v >= 0.0f);
        REQUIRE(v <= 1.0f);
    }
}

TEST_CASE("SpotRemoval: overlapping Clone source reads pre-spot pixels",
          "[spotremoval]") {
    const int w = 32, h = 32;
    std::vector<float> px(static_cast<size_t>(w) * h * 3);
    // Horizontal gradient: pixel value = x/31 (identical on all channels).
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                px[(static_cast<size_t>(y) * w + x) * 3 + c] =
                    static_cast<float>(x) / 31.0f;

    // Source (0.4, 0.5) and target (0.55, 0.5) overlap heavily: the radius is
    // 0.3*32 = 9.6 px and the centres are 4.8 px apart. The target-centre
    // pixel (18, 16) samples source (13.2, 16); its source neighbourhood is
    // itself a target pixel, so a correct implementation must read the PRE-SPOT
    // gradient (~0.426) rather than the already-painted value (~0.27).
    Adjustments a;
    SpotRemoval s;
    s.type = SpotType::Clone;
    s.sourceX = 0.4f;
    s.sourceY = 0.5f;
    s.targetX = 0.55f;
    s.targetY = 0.5f;
    s.radius = 0.3f;
    a.spotRemovals.push_back(s);
    Pipeline::applySpotRemoval(a, px.data(), w, h);

    const auto c = rgbAt(px, w, 18, 16);
    REQUIRE(c[0] > 0.35f);  // pre-spot gradient, not the overwritten ~0.27
    REQUIRE(c[0] < 0.55f);
}

TEST_CASE("SpotRemoval: out-of-bounds source pixels are skipped",
          "[spotremoval]") {
    const int w = 16, h = 16;
    std::vector<float> px(static_cast<size_t>(w) * h * 3, 0.25f);
    const std::vector<float> original = px;

    Adjustments a;
    SpotRemoval s;
    s.type = SpotType::Clone;
    s.sourceX = -0.5f;
    s.sourceY = -0.5f;  // source fully outside the image
    s.targetX = 0.5f;
    s.targetY = 0.5f;
    s.radius = 0.4f;
    a.spotRemovals.push_back(s);

    Pipeline::applySpotRemoval(a, px.data(), w, h);
    // Every mapped source sample is out of bounds, so no pixel may change.
    REQUIRE(px == original);
}

TEST_CASE("SpotRemoval: empty or invisible spots are a bit-exact no-op",
          "[spotremoval]") {
    const int w = 32, h = 32;
    std::vector<float> px(static_cast<size_t>(w) * h * 3);
    // Deterministic pseudo-random content in [0,1].
    uint32_t seed = 12345u;
    for (float& v : px) {
        seed = seed * 1664525u + 1013904223u;
        v = static_cast<float>(seed >> 8) / 16777216.0f;
    }
    const std::vector<float> original = px;

    // Empty vector: applySpotRemoval must not touch a single byte.
    Pipeline::applySpotRemoval(Adjustments{}, px.data(), w, h);
    REQUIRE(px == original);

    // An all-invisible spot list behaves exactly like an empty one through the
    // spatial pipeline as well.
    Adjustments aHidden;
    SpotRemoval hidden;
    hidden.type = SpotType::Clone;
    hidden.sourceX = 0.1f;
    hidden.sourceY = 0.1f;
    hidden.targetX = 0.9f;
    hidden.targetY = 0.9f;
    hidden.radius = 0.3f;
    hidden.visible = false;
    aHidden.spotRemovals.push_back(hidden);

    std::vector<float> b1 = original, b2 = original;
    Pipeline::applySpatialAdjustments(Adjustments{}, b1.data(), w, h);
    Pipeline::applySpatialAdjustments(aHidden, b2.data(), w, h);
    REQUIRE(b1 == b2);
    REQUIRE(b1 == original);  // neutral stack clamps [0,1] values in place
}

TEST_CASE("SpotRemoval: normalized coordinates land consistently at two sizes",
          "[spotremoval]") {
    auto markerAtTargetCentre = [](int size) -> float {
        std::vector<float> px(static_cast<size_t>(size) * size * 3, 0.5f);
        // A single bright marker at the source centre (0.25, 0.5).
        const int mx = static_cast<int>(std::lround(0.25f * size));
        const int my = static_cast<int>(std::lround(0.5f * size));
        float* m = px.data() + (static_cast<size_t>(my) * size + mx) * 3;
        m[0] = 1.0f;
        m[1] = 1.0f;
        m[2] = 1.0f;

        Adjustments a;
        SpotRemoval s;
        s.type = SpotType::Clone;
        s.sourceX = 0.25f;
        s.sourceY = 0.5f;
        s.targetX = 0.5f;
        s.targetY = 0.5f;
        s.radius = 0.3f;
        a.spotRemovals.push_back(s);
        Pipeline::applySpotRemoval(a, px.data(), size, size);

        // The marker is copied to the target centre (0.5 * size): the pixel at
        // offset 0 samples the source centre exactly.
        const int tc = size / 2;
        return px[(static_cast<size_t>(tc) * size + tc) * 3 + 0];
    };

    // Same normalized parameters produce the same relative result at 64 and
    // 128 px: the source marker appears at the normalized (0.5, 0.5) position.
    REQUIRE(markerAtTargetCentre(64) > 0.9f);
    REQUIRE(markerAtTargetCentre(128) > 0.9f);
}

TEST_CASE("SpotRemoval: spotRemovals participates in the Adjustments hash",
          "[spotremoval]") {
    Adjustments a;
    const size_t h0 = a.hash();

    SpotRemoval s;
    s.type = SpotType::Clone;
    s.sourceX = 0.2f;
    s.sourceY = 0.3f;
    s.targetX = 0.7f;
    s.targetY = 0.6f;
    s.radius = 0.15f;
    a.spotRemovals.push_back(s);
    const size_t h1 = a.hash();
    REQUIRE(h1 != h0);

    // Changing a single field changes the hash.
    Adjustments b = a;
    b.spotRemovals[0].radius = 0.2f;
    REQUIRE(b.hash() != h1);

    // operator== reflects the field too.
    REQUIRE(a != b);
    REQUIRE(a == a);

    // The stage-E (spatial) hash reacts to a spot change so the incremental
    // preview cache restarts at stage E; stages A-D are unaffected.
    REQUIRE(b.hashStageE() != a.hashStageE());
    REQUIRE(a.hashStageA() == b.hashStageA());
    REQUIRE(a.hashStageB() == b.hashStageB());
    REQUIRE(a.hashStageC() == b.hashStageC());
    REQUIRE(a.hashStageD() == b.hashStageD());

    // Toggling visibility and the type both change the hash.
    Adjustments c = a;
    c.spotRemovals[0].type = SpotType::Heal;
    REQUIRE(c.hash() != h1);
    Adjustments d = a;
    d.spotRemovals[0].visible = false;
    REQUIRE(d.hash() != h1);
}
