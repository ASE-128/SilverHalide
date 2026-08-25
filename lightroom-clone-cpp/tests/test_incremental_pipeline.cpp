// PHASE_2 incremental preview pipeline tests.
//
// Covers:
//   - renderPreview at preview (2048px) and full resolution is visually
//     consistent (identical when the standard preview is the exact area-average
//     downsample; the allowed <1% pixel tolerance is asserted).
//   - Incremental render-state caching: only the first changed stage is
//     recomputed (verified via Pipeline::lastRecomputeStage / lastReuseStartStage).
//   - Spatial-kernel sigma scaling by the preview/full-resolution ratio
//     (Pipeline::spatialKernels / Pipeline::previewSigmaScale).
//   - Neutral parameter fast path: renderPreview with default adjustments equals
//     a plain resize + sRGB conversion.
//   - The incremental cache honours its memory budget.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "engine/ColorManager.h"
#include "engine/ImageBuffer.h"
#include "engine/Pipeline.h"

using namespace Catch;
using namespace lrc;

namespace {

// A smooth gradient with colour structure so both tonal and spatial stages have
// content to act on.
ImageBufferPtr makeGradientImage(int w, int h) {
    auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
    for (int y = 0; y < h; ++y) {
        const float v = (h > 1) ? static_cast<float>(y) / (h - 1) : 0.0f;
        for (int x = 0; x < w; ++x) {
            const float u = (w > 1) ? static_cast<float>(x) / (w - 1) : 0.0f;
            buf->setPixel(x, y, 0, u * 0.8f + v * 0.2f);
            buf->setPixel(x, y, 1, 0.2f + 0.6f * v);
            buf->setPixel(x, y, 2, 0.3f + 0.5f * u);
        }
    }
    return buf;
}

// A parameter stack that touches every stage (A basic tonal, C WB/dehaze,
// E spatial) but keeps the tone curve (B) and the HSL/split/point-colour (D)
// neutral.
Adjustments sampleAdjustments() {
    Adjustments a;
    a.exposure = 0.3;
    a.contrast = 15.0;
    a.highlights = -20.0;
    a.shadows = 10.0;
    a.whites = 5.0;
    a.blacks = -8.0;
    a.temperature = 8.0;
    a.dehaze = 5.0;
    a.texture = 35.0;
    a.clarity = 25.0;
    a.sharpness = 60.0;
    a.sharpnessRadius = 1.2;
    a.grain = 20.0;
    a.vignette = -15.0;
    return a;
}

// Counts pixels whose any channel differs by more than `threshold` (0..255).
size_t countDiffPixels(const ImageBufferPtr& a, const ImageBufferPtr& b,
                       int threshold) {
    if (!a || !b || a->width() != b->width() || a->height() != b->height() ||
        a->format() != b->format())
        return static_cast<size_t>(-1);
    const uint8_t* pa = a->data().data();
    const uint8_t* pb = b->data().data();
    const size_t n = static_cast<size_t>(a->width()) * a->height() * 3;
    size_t diff = 0;
    for (size_t i = 0; i < n; i += 3) {
        const int dr = std::abs(static_cast<int>(pa[i]) - static_cast<int>(pb[i]));
        const int dg =
            std::abs(static_cast<int>(pa[i + 1]) - static_cast<int>(pb[i + 1]));
        const int db =
            std::abs(static_cast<int>(pa[i + 2]) - static_cast<int>(pb[i + 2]));
        if (dr > threshold || dg > threshold || db > threshold)
            ++diff;
    }
    return diff;
}

} // namespace

TEST_CASE("renderPreview: preview resolution matches full resolution",
          "[incremental][renderPreview]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);

    constexpr int fullW = 2048;
    constexpr int fullH = 1365;
    constexpr int prevW = 1024;
    constexpr int prevH = 683;
    auto full = makeGradientImage(fullW, fullH);
    // The import-time standard preview: an area-average downsample of the full
    // buffer, exactly what CatalogDatabase stores for a 2048px-long-edge
    // standard preview (here stored at the preview target size).
    auto standard = full->resizeToF32(prevW, prevH);

    const Size target{prevW, prevH};
    const Adjustments adj = sampleAdjustments();

    // Feed the full-resolution source: renderPreview downsizes internally.
    auto fromFull = pipeline.renderPreview(*full, adj, target, fullW, 7);
    REQUIRE(fromFull);
    REQUIRE(fromFull->format() == PixelFormat::U8);
    REQUIRE(fromFull->width() == prevW);
    REQUIRE(fromFull->height() == prevH);

    // Feed the standard preview (already at the target resolution). Distinct
    // image id so the cache treats this as a fresh source (full recompute).
    auto fromStandard = pipeline.renderPreview(*standard, adj, target, fullW, 8);
    REQUIRE(fromStandard);
    REQUIRE(fromStandard->format() == PixelFormat::U8);

    const size_t total = static_cast<size_t>(prevW) * prevH;
    const size_t diff = countDiffPixels(fromFull, fromStandard, 8);
    REQUIRE(diff < total / 100);  // < 1% pixels differ
}

TEST_CASE("renderPreview: incremental cache recomputes from the first changed "
          "stage", "[incremental][renderPreview]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    auto src = makeGradientImage(1024, 683);
    const Size target{1024, 683};
    constexpr uint64_t imageId = 42;

    Adjustments base = sampleAdjustments();

    // First render: the whole pipeline runs.
    auto r1 = pipeline.renderPreview(*src, base, target, 1024, imageId);
    REQUIRE((r1 && !r1->empty()));
    REQUIRE(pipeline.lastRecomputeStage() == 0);
    REQUIRE(pipeline.lastReuseStartStage() == -1);

    // Stage D only: HSL red saturation. Must restart from D (3), reusing the
    // cached A, B, C outputs (C's output is the input to D).
    Adjustments hsl = base;
    hsl.hsl[static_cast<size_t>(ColorRange::Red)].saturation = 40.0;
    auto r2 = pipeline.renderPreview(*src, hsl, target, 1024, imageId);
    REQUIRE((r2 && !r2->empty()));
    REQUIRE(pipeline.lastRecomputeStage() == 3);  // Stage D
    REQUIRE(pipeline.lastReuseStartStage() == 2); // input = cached Stage C output

    // Stage A only: exposure. Must restart from A (0).
    Adjustments exp = base;
    exp.exposure += 0.5;
    auto r3 = pipeline.renderPreview(*src, exp, target, 1024, imageId);
    REQUIRE((r3 && !r3->empty()));
    REQUIRE(pipeline.lastRecomputeStage() == 0);
    REQUIRE(pipeline.lastReuseStartStage() == -1);

    // Identical adjustments: served entirely from the cache (same final buffer).
    auto r4 = pipeline.renderPreview(*src, exp, target, 1024, imageId);
    REQUIRE(r4.get() == r3.get());
    REQUIRE(pipeline.lastRecomputeStage() == -1);

    // A different image id (source change) forces a full recompute from A.
    auto r5 = pipeline.renderPreview(*src, exp, target, 1024, imageId + 1);
    REQUIRE((r5 && !r5->empty()));
    REQUIRE(pipeline.lastRecomputeStage() == 0);
}

TEST_CASE("renderPreview: spatial kernels scale with the preview/full ratio",
          "[incremental][renderPreview]") {
    Adjustments a;
    a.texture = 50.0;
    a.clarity = 40.0;
    a.sharpness = 80.0;
    a.sharpnessRadius = 2.0;
    a.grainSize = 30.0;

    const Pipeline::SpatialKernels k1 =
        Pipeline::spatialKernels(a, 2048, 1365, 1.0f);
    REQUIRE(k1.textureSigma == Approx(2.5f));
    // clarityRadius is buffer-relative (max(4, min(w,h)/50)) and NOT scaled.
    REQUIRE(k1.clarityRadius == Approx(std::max(4.0f, 1365.0f / 50.0f)));
    REQUIRE(k1.sharpnessSigma == Approx(2.0f));
    REQUIRE(k1.grainCell == Approx(3.0f + 30.0f * 0.45f));

    // Half resolution: absolute-pixel kernels halve, clarity stays put.
    const Pipeline::SpatialKernels kHalf =
        Pipeline::spatialKernels(a, 2048, 1365, 0.5f);
    REQUIRE(kHalf.textureSigma == Approx(1.25f));
    REQUIRE(kHalf.sharpnessSigma == Approx(1.0f));
    REQUIRE(kHalf.grainCell == Approx(k1.grainCell * 0.5f));
    REQUIRE(kHalf.clarityRadius == Approx(k1.clarityRadius));

    // The scale factor itself.
    REQUIRE(Pipeline::previewSigmaScale(2048, 4096) == Approx(0.5f));
    REQUIRE(Pipeline::previewSigmaScale(1024, 2048) == Approx(0.5f));
    REQUIRE(Pipeline::previewSigmaScale(2048, 0) == Approx(1.0f));   // unknown
    REQUIRE(Pipeline::previewSigmaScale(2048, 1024) == Approx(1.0f)); // no upscale
}

TEST_CASE("renderPreview: neutral adjustments equal a plain resize + sRGB",
          "[incremental][renderPreview]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    auto src = makeGradientImage(1000, 500);  // aspect 2.0
    const Size target{500, 250};              // exact half, aspect 2.0

    auto out = pipeline.renderPreview(*src, Adjustments{}, target, 1000, 1);
    REQUIRE(out);
    REQUIRE(out->format() == PixelFormat::U8);
    REQUIRE(out->width() == 500);
    REQUIRE(out->height() == 250);

    // Expected: source resized to the target, converted to display sRGB.
    auto resized = src->resizeToF32(500, 250);
    auto expected = cm->toDisplaySRGB(*resized);
    REQUIRE(out->sizeBytes() == expected->sizeBytes());
    const uint8_t* a = out->data().data();
    const uint8_t* b = expected->data().data();
    const size_t n = out->sizeBytes();
    for (size_t i = 0; i < n; ++i)
        REQUIRE(a[i] == b[i]);
}

TEST_CASE("renderPreview: incremental cache honours the memory budget",
          "[incremental][renderPreview]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    // 3 MB: each F32 stage at 1024x683 is ~8.4 MB, so eviction must kick in and
    // drop the non-essential stages, leaving only the final sRGB frame (~2 MB).
    constexpr size_t budget = 3u * 1024u * 1024u;
    pipeline.setIncrementalCacheCapacity(budget);

    auto src = makeGradientImage(1024, 683);
    const Size target{1024, 683};
    Adjustments base = sampleAdjustments();

    auto r1 = pipeline.renderPreview(*src, base, target, 1024, 1);
    REQUIRE((r1 && !r1->empty()));
    REQUIRE(pipeline.incrementalCacheBytes() <= budget);

    // A stage-D change still renders correctly after eviction (it falls back to
    // recomputing from an earlier retained stage / the source).
    Adjustments hsl = base;
    hsl.hsl[static_cast<size_t>(ColorRange::Red)].saturation = 30.0;
    auto r2 = pipeline.renderPreview(*src, hsl, target, 1024, 1);
    REQUIRE((r2 && !r2->empty()));
    REQUIRE(pipeline.incrementalCacheBytes() <= budget);
}
