#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "engine/ColorManager.h"
#include "engine/ImageBuffer.h"
#include "engine/Pipeline.h"
#include "engine/RenderScratch.h"
#include "app/Constants.h"

using namespace Catch;
using namespace lrc;

namespace lrc {
// Test-only hook into the internal single-channel blur defined in Pipeline.cpp
// (gaussianBlur is file-local; this wrapper exposes it to the tests).
std::vector<float> pipelineGaussianBlurTest(const float* src, int w, int h,
                                            float sigma);
}

TEST_CASE("Pipeline: exposure doubles the linear signal", "[pipeline]") {
    std::vector<float> px{0.18f, 0.18f, 0.18f};
    Adjustments a;
    a.exposure = 1.0;
    Pipeline::applyAdjustments(a, px.data(), 1);
    REQUIRE(px[0] == Approx(0.36f).margin(0.01f));
    REQUIRE(px[1] == Approx(0.36f).margin(0.01f));
    REQUIRE(px[2] == Approx(0.36f).margin(0.01f));
}

TEST_CASE("Pipeline: negative exposure darkens", "[pipeline]") {
    std::vector<float> px{0.5f, 0.5f, 0.5f};
    Adjustments a;
    a.exposure = -1.0;
    Pipeline::applyAdjustments(a, px.data(), 1);
    REQUIRE(px[0] == Approx(0.25f).margin(0.01f));
}

TEST_CASE("Pipeline: saturation increases channel separation", "[pipeline]") {
    std::vector<float> px{1.0f, 0.5f, 0.0f};
    Adjustments a;
    a.saturation = 100.0;
    Pipeline::applyAdjustments(a, px.data(), 1);
    REQUIRE(px[0] > 0.9f);
    REQUIRE(px[1] < 0.5f);
    REQUIRE(px[2] < 0.5f);
}

TEST_CASE("Pipeline: default adjustments are a no-op for neutral gray",
          "[pipeline]") {
    std::vector<float> px{0.4f, 0.4f, 0.4f};
    Pipeline::applyAdjustments(Adjustments::defaults(), px.data(), 1);
    REQUIRE(px[0] == Approx(0.4f).margin(1e-3f));
    REQUIRE(px[1] == Approx(0.4f).margin(1e-3f));
    REQUIRE(px[2] == Approx(0.4f).margin(1e-3f));
}

TEST_CASE("Pipeline: temperature warm shift boosts red", "[pipeline]") {
    std::vector<float> px{0.5f, 0.5f, 0.5f};
    Adjustments a;
    a.temperature = 100.0;
    Pipeline::applyAdjustments(a, px.data(), 1);
    REQUIRE(px[0] > px[2]);
}

TEST_CASE("Pipeline: highlights boost bright regions", "[pipeline]") {
    // Bright pixel (0.8) should be lifted more than mid-gray (0.18).
    std::vector<float> px{0.8f, 0.8f, 0.8f, 0.18f, 0.18f, 0.18f};
    Adjustments a;
    a.highlights = 100.0;
    Pipeline::applyAdjustments(a, px.data(), 2);
    // Bright pixel lifted significantly.
    REQUIRE(px[0] > 0.85f);
    // Mid-gray barely affected.
    REQUIRE(px[3] == Approx(0.18f).margin(0.02f));
}

TEST_CASE("Pipeline: shadows boost dark regions", "[pipeline]") {
    // Dark pixel (0.02) should be lifted more than mid-gray (0.18).
    std::vector<float> px{0.02f, 0.02f, 0.02f, 0.18f, 0.18f, 0.18f};
    Adjustments a;
    a.shadows = 100.0;
    Pipeline::applyAdjustments(a, px.data(), 2);
    // Dark pixel lifted (output ~0.03, allow rounding margin).
    REQUIRE(px[0] > 0.029f);
    // Mid-gray is also lifted by the shadows smooth transition zone
    // (shadows blend from 0 to 0.35), so compare ratios instead.
    const float darkRatio = px[0] / 0.02f;
    const float midRatio = px[3] / 0.18f;
    REQUIRE(darkRatio > midRatio);
}

TEST_CASE("Pipeline: whites lifts bright regions, mid-tones untouched",
          "[pipeline]") {
    std::vector<float> px{0.9f, 0.9f, 0.9f, 0.18f, 0.18f, 0.18f};
    Adjustments a;
    a.whites = 100.0;
    Pipeline::applyAdjustments(a, px.data(), 2);
    // Bright pixels pushed toward clipping.
    REQUIRE(px[0] > 0.9f);
    REQUIRE(px[1] > 0.9f);
    REQUIRE(px[2] > 0.9f);
    // Mid-gray essentially untouched.
    REQUIRE(px[3] == Approx(0.18f).margin(0.02f));
}

TEST_CASE("Pipeline: blacks crushes shadows, mid-tones untouched",
          "[pipeline]") {
    std::vector<float> px{0.02f, 0.02f, 0.02f, 0.18f, 0.18f, 0.18f};
    Adjustments a;
    a.blacks = 100.0;
    Pipeline::applyAdjustments(a, px.data(), 2);
    // Shadow pixels crushed toward black.
    REQUIRE(px[0] < 0.02f);
    REQUIRE(px[1] < 0.02f);
    REQUIRE(px[2] < 0.02f);
    // Mid-gray essentially untouched.
    REQUIRE(px[3] == Approx(0.18f).margin(0.02f));
}

TEST_CASE("Pipeline: negative whites darkens highlights", "[pipeline]") {
    std::vector<float> px{0.9f, 0.9f, 0.9f};
    Adjustments a;
    a.whites = -100.0;
    Pipeline::applyAdjustments(a, px.data(), 1);
    REQUIRE(px[0] < 0.85f);
}

TEST_CASE("Pipeline: grain perturbs pixels deterministically", "[pipeline]") {
    const int w = 16, h = 16;
    std::vector<float> px(static_cast<size_t>(w) * h * 3, 0.5f);
    Adjustments a;
    a.grain = 100.0;
    a.grainSize = 25.0;
    a.grainRoughness = 50.0;
    Pipeline::applySpatialAdjustments(a, px.data(), w, h);

    // Deterministic: identical input + parameters -> identical output.
    std::vector<float> px2(static_cast<size_t>(w) * h * 3, 0.5f);
    Pipeline::applySpatialAdjustments(a, px2.data(), w, h);
    REQUIRE(px == px2);

    // Some pixel differs from the flat 0.5 input.
    bool changed = false;
    for (size_t i = 0; i < px.size(); i += 3)
        if (std::abs(px[i] - 0.5f) > 1e-4f) {
            changed = true;
            break;
        }
    REQUIRE(changed);
}

TEST_CASE("Pipeline: negative highlights suppress brights", "[pipeline]") {
    std::vector<float> px{0.9f, 0.9f, 0.9f};
    Adjustments a;
    a.highlights = -100.0;
    Pipeline::applyAdjustments(a, px.data(), 1);
    REQUIRE(px[0] < 0.8f);
}

TEST_CASE("Pipeline: render produces an 8-bit preview at maxDim", "[pipeline]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);

    // Simple vertical gradient, F32 linear ProPhoto.
    auto buf = ImageBuffer::create(200, 100, PixelFormat::F32);
    for (int y = 0; y < 100; ++y) {
        const float v = static_cast<float>(y) / 99.0f;
        for (int x = 0; x < 200; ++x) {
            buf->setPixel(x, y, 0, v);
            buf->setPixel(x, y, 1, v * 0.8f);
            buf->setPixel(x, y, 2, v * 0.6f);
        }
    }

    auto out = pipeline.render(7, *buf, Adjustments::defaults(), 50);
    REQUIRE(out->format() == PixelFormat::U8);
    REQUIRE(out->width() == 50);
    REQUIRE(out->height() == 25);

    // The same render is served from the LRU cache (identical key).
    auto cached = pipeline.render(7, *buf, Adjustments::defaults(), 50);
    REQUIRE(cached->data().data() == out->data().data());
}

TEST_CASE("Pipeline: renderLinearBounded stays linear F32 at a bounded size",
          "[pipeline]") {
    // Soft proofing feeds its colourimetric transform from this entry point: it
    // must be the same stage render() converts with toDisplaySRGB (so a proofed
    // frame and an unproofed frame of one edit differ only by the proof
    // transform), at a bounded size and without touching the display LRU.
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);

    auto buf = ImageBuffer::create(64, 32, PixelFormat::F32);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 64; ++x) {
            buf->setPixel(x, y, 0, 0.25f);
            buf->setPixel(x, y, 1, static_cast<float>(y) / 31.0f);
            buf->setPixel(x, y, 2, 0.25f);
        }
    const Adjustments adj = Adjustments::defaults();

    // maxDim <= 0 keeps the source resolution.
    auto full = pipeline.renderLinearBounded(*buf, adj, 0);
    REQUIRE(full);
    CHECK(full->format() == PixelFormat::F32);
    CHECK(full->width() == 64);
    CHECK(full->height() == 32);
    // The source must be left untouched (applyAdjustments edits in place).
    CHECK(buf->pixel(0, 0, 0) == Approx(0.25f));

    auto half = pipeline.renderLinearBounded(*buf, adj, 16);
    REQUIRE(half);
    CHECK(half->format() == PixelFormat::F32);
    CHECK(half->width() == 16);
    CHECK(half->height() == 8);

    // The contract the proof path depends on: this buffer IS the stage render()
    // converts, so converting it with the display transform must reproduce that
    // render (the numeric values are not comparable directly — the buffer is
    // linear ProPhoto while the render is display-encoded sRGB).
    auto display = pipeline.render(0, *buf, adj, 16);
    REQUIRE(display);
    REQUIRE(display->format() == PixelFormat::U8);
    auto converted = cm->toDisplaySRGB(*half);
    REQUIRE(converted);
    REQUIRE(converted->width() == display->width());
    REQUIRE(converted->height() == display->height());
    int maxDiff = 0;
    for (size_t i = 0; i < converted->data().size(); ++i)
        maxDiff = std::max(maxDiff, std::abs(static_cast<int>(converted->data()[i]) -
                                             static_cast<int>(display->data()[i])));
    CHECK(maxDiff <= 1);
}

TEST_CASE("Pipeline: cache is invalidated by a parameter change", "[pipeline]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    auto buf = ImageBuffer::create(10, 10, PixelFormat::F32);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x) {
            buf->setPixel(x, y, 0, 0.3f);
            buf->setPixel(x, y, 1, 0.3f);
            buf->setPixel(x, y, 2, 0.3f);
        }

    Adjustments bright;
    bright.exposure = 1.0;
    Adjustments dark;
    dark.exposure = -1.0;

    const auto a = pipeline.render(1, *buf, bright, 10);
    const auto b = pipeline.render(1, *buf, dark, 10);
    REQUIRE(a->data().data() != b->data().data());
    REQUIRE(a->pixel(0, 0, 0) > b->pixel(0, 0, 0));
}

// --- HSL per-channel tests ---------------------------------------------------

TEST_CASE("Pipeline: HSL per-channel red saturation boost affects red pixels",
          "[pipeline][hsl]") {
    // Pure red pixel: should be strongly affected by red saturation boost.
    std::vector<float> px{1.0f, 0.0f, 0.0f};
    Adjustments a;
    a.hsl[static_cast<size_t>(ColorRange::Red)].saturation = 100.0;
    Pipeline::applyAdjustments(a, px.data(), 1);
    // Red should be the dominant channel.
    REQUIRE(px[0] > 0.6f);
    REQUIRE(px[1] < 0.3f);
    REQUIRE(px[2] < 0.3f);
}

TEST_CASE("Pipeline: HSL per-channel desaturation mutes colour",
          "[pipeline][hsl]") {
    std::vector<float> px{1.0f, 0.0f, 0.0f};
    Adjustments a;
    a.hsl[static_cast<size_t>(ColorRange::Red)].saturation = -100.0;
    Pipeline::applyAdjustments(a, px.data(), 1);
    // Red should be desaturated toward grey — all channels closer together.
    REQUIRE(px[0] > 0.0f);
    REQUIRE(px[1] > 0.0f);
    REQUIRE(px[2] > 0.0f);
    // The difference between max and min should be reduced.
    const float range = std::max({px[0], px[1], px[2]}) -
                        std::min({px[0], px[1], px[2]});
    REQUIRE(range < 0.5f);
}

TEST_CASE("Pipeline: HSL per-channel hue shift changes colour",
          "[pipeline][hsl]") {
    std::vector<float> px{1.0f, 0.0f, 0.0f}; // red (hue ~0)
    Adjustments a;
    // Shift red hue toward yellow (+30°).
    a.hsl[static_cast<size_t>(ColorRange::Red)].hue = 30.0;
    Pipeline::applyAdjustments(a, px.data(), 1);
    // The result should have more green than the original red.
    REQUIRE(px[1] > 0.3f);
}

TEST_CASE("Pipeline: HSL per-channel luminance boost", "[pipeline][hsl]") {
    std::vector<float> px{0.5f, 0.3f, 0.1f};
    Adjustments a;
    a.hsl[static_cast<size_t>(ColorRange::Orange)].luminance = 100.0;
    Pipeline::applyAdjustments(a, px.data(), 1);
    // The pixel contains significant orange, so luminance should increase.
    const float luma = 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
    const float origLuma = 0.2126f * 0.5f + 0.7152f * 0.3f + 0.0722f * 0.1f;
    REQUIRE(luma > origLuma);
}

TEST_CASE("Pipeline: HSL per-channel zero is a no-op for neutral grey",
          "[pipeline][hsl]") {
    std::vector<float> px{0.4f, 0.4f, 0.4f};
    Adjustments a;
    // Set some non-zero HSL but all at zero.
    a.hsl[static_cast<size_t>(ColorRange::Red)].hue = 0.0;
    a.hsl[static_cast<size_t>(ColorRange::Green)].saturation = 0.0;
    a.hsl[static_cast<size_t>(ColorRange::Blue)].luminance = 0.0;
    Pipeline::applyAdjustments(a, px.data(), 1);
    REQUIRE(px[0] == Approx(0.4f).margin(1e-3f));
    REQUIRE(px[1] == Approx(0.4f).margin(1e-3f));
    REQUIRE(px[2] == Approx(0.4f).margin(1e-3f));
}

// --- Split toning tests ------------------------------------------------------

TEST_CASE("Pipeline: split toning adds colour to highlights", "[pipeline][split]") {
    std::vector<float> px{0.9f, 0.9f, 0.9f}; // bright pixel
    Adjustments a;
    a.splitTone.highlightHue = 0.0;  // red tint
    a.splitTone.highlightSaturation = 50.0;
    Pipeline::applyAdjustments(a, px.data(), 1);
    // Highlights should shift toward red.
    REQUIRE(px[0] > px[1]);
    REQUIRE(px[0] > px[2]);
}

TEST_CASE("Pipeline: split toning shadows are blue-tinted", "[pipeline][split]") {
    std::vector<float> px{0.05f, 0.05f, 0.05f}; // dark pixel
    Adjustments a;
    a.splitTone.shadowHue = 240.0;  // blue tint
    a.splitTone.shadowSaturation = 50.0;
    Pipeline::applyAdjustments(a, px.data(), 1);
    // Shadows should shift toward blue.
    REQUIRE(px[2] > px[0]);
    REQUIRE(px[2] > px[1]);
}

TEST_CASE("Pipeline: split toning balance shifts the transition",
          "[pipeline][split]") {
    Adjustments a;
    a.splitTone.highlightHue = 0.0;    // red
    a.splitTone.highlightSaturation = 100.0;
    a.splitTone.shadowHue = 240.0;     // blue
    a.splitTone.shadowSaturation = 100.0;

    // balance = 0: a mid-tone pixel sits on the boundary; red + blue tints
    // cancel on R/B but remove the green channel.
    std::vector<float> px{0.5f, 0.5f, 0.5f};
    Pipeline::applyAdjustments(a, px.data(), 1);
    REQUIRE(px[1] < 0.5f);

    // balance = -100: more highlight tone -> mid-tone takes the red tint.
    std::vector<float> pxRed{0.5f, 0.5f, 0.5f};
    a.splitTone.balance = -100.0;
    Pipeline::applyAdjustments(a, pxRed.data(), 1);
    REQUIRE(pxRed[0] > 0.5f);
    REQUIRE(pxRed[0] > pxRed[2]);

    // balance = +100: more shadow tone -> mid-tone takes the blue tint.
    std::vector<float> pxBlue{0.5f, 0.5f, 0.5f};
    a.splitTone.balance = 100.0;
    Pipeline::applyAdjustments(a, pxBlue.data(), 1);
    REQUIRE(pxBlue[2] > 0.5f);
    REQUIRE(pxBlue[2] > pxBlue[0]);
}

TEST_CASE("Pipeline: split toning zero saturation is a no-op",
          "[pipeline][split]") {
    std::vector<float> px{0.7f, 0.7f, 0.7f};
    Adjustments a;
    a.splitTone.highlightHue = 0.0;
    a.splitTone.highlightSaturation = 0.0;
    a.splitTone.shadowHue = 240.0;
    a.splitTone.shadowSaturation = 0.0;
    Pipeline::applyAdjustments(a, px.data(), 1);
    REQUIRE(px[0] == Approx(0.7f).margin(1e-3f));
    REQUIRE(px[1] == Approx(0.7f).margin(1e-3f));
    REQUIRE(px[2] == Approx(0.7f).margin(1e-3f));
}

// --- Alt-key visualization overlays -----------------------------------------

namespace {

ImageBufferPtr makeVisualTestBuffer() {
    // 64x64 linear gradient: dark top-left -> bright bottom-right.
    auto buf = ImageBuffer::create(64, 64, PixelFormat::F32);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const float v = static_cast<float>(x + y) / 126.0f;
            buf->setPixel(x, y, 0, v);
            buf->setPixel(x, y, 1, v);
            buf->setPixel(x, y, 2, v);
        }
    return buf;
}

} // namespace

TEST_CASE("Pipeline: clipping overlay marks blown highlights red",
          "[pipeline][visualize]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    auto buf = ImageBuffer::create(8, 8, PixelFormat::F32);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const float v = (x < 4) ? 0.2f : 1.0f; // right half fully blown
            buf->setPixel(x, y, 0, v);
            buf->setPixel(x, y, 1, v);
            buf->setPixel(x, y, 2, v);
        }
    Adjustments a;
    a.exposure = 2.0; // push highlights into clipping
    auto ov = pipeline.visualizeOverlay(*buf, a, Pipeline::VisualizeMode::Clipping, 0);
    REQUIRE(ov != nullptr);
    REQUIRE(ov->format() == PixelFormat::U8);
    REQUIRE(ov->width() == 8);
    REQUIRE(ov->height() == 8);
    // Bottom-right pixel (blown) should be strongly red.
    const float r = ov->pixel(7, 7, 0);
    const float g = ov->pixel(7, 7, 1);
    REQUIRE(r > 0.9f);
    REQUIRE(g < 0.1f);
}

TEST_CASE("Pipeline: clipping overlay marks crushed shadows blue",
          "[pipeline][visualize]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    auto buf = ImageBuffer::create(8, 8, PixelFormat::F32);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const float v = (x < 4) ? 0.0f : 0.5f;
            buf->setPixel(x, y, 0, v);
            buf->setPixel(x, y, 1, v);
            buf->setPixel(x, y, 2, v);
        }
    Adjustments a;
    a.exposure = -4.0; // crush everything into black
    auto ov = pipeline.visualizeOverlay(*buf, a, Pipeline::VisualizeMode::Clipping, 0);
    REQUIRE(ov != nullptr);
    // Top-left (crushed) pixel should be blue.
    const float b = ov->pixel(0, 0, 2);
    const float r = ov->pixel(0, 0, 0);
    REQUIRE(b > 0.9f);
    REQUIRE(r < 0.1f);
}

TEST_CASE("Pipeline: detail mask is a bounded grayscale buffer",
          "[pipeline][visualize]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    auto buf = makeVisualTestBuffer();
    Adjustments a;
    a.clarity = 50.0;
    auto ov = pipeline.visualizeOverlay(*buf, a, Pipeline::VisualizeMode::DetailMask, 32);
    REQUIRE(ov != nullptr);
    REQUIRE(ov->width() == 32);
    REQUIRE(ov->height() == 32);
    // Every pixel is neutral grey (r == g == b) and in range.
    for (int y = 0; y < 32; y += 4)
        for (int x = 0; x < 32; x += 4) {
            const float r = ov->pixel(x, y, 0);
            const float g = ov->pixel(x, y, 1);
            const float b = ov->pixel(x, y, 2);
            REQUIRE(std::abs(r - g) < 1e-3f);
            REQUIRE(std::abs(g - b) < 1e-3f);
        }
}

TEST_CASE("Pipeline: edge mask is binary and shrinks with masking",
          "[pipeline][visualize]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    auto buf = ImageBuffer::create(32, 32, PixelFormat::F32);
    // Black field with a bright stripe (x=8, v=1.0) and a dimmer stripe
    // (x=24, v=0.4). Two edge strengths let the masking threshold separate
    // them: with high masking only the strong edge survives.
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const float v = (x == 8)   ? 1.0f
                            : (x == 24) ? 0.4f
                                        : 0.0f;
            buf->setPixel(x, y, 0, v);
            buf->setPixel(x, y, 1, v);
            buf->setPixel(x, y, 2, v);
        }
    Adjustments a; // masking 0 -> every edge sharpened
    auto low = pipeline.visualizeOverlay(*buf, a, Pipeline::VisualizeMode::EdgeMask, 0);
    a.sharpnessMasking = 90.0; // masking 90 -> very few pixels sharpened
    auto high = pipeline.visualizeOverlay(*buf, a, Pipeline::VisualizeMode::EdgeMask, 0);
    REQUIRE(low != nullptr);
    REQUIRE(high != nullptr);

    int lowCount = 0, highCount = 0;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            if (low->pixel(x, y, 0) > 0.9f) ++lowCount;
            if (high->pixel(x, y, 0) > 0.9f) ++highCount;
        }
    REQUIRE(lowCount > 0);
    REQUIRE(highCount < lowCount);
}

// --- M2.1 full-resolution (maxDim=0) paths ----------------------------------

namespace {

// 8x8 F32 linear gradient in [0.05, 0.5]. Kept below 1.0 so a +0.5 stop
// exposure gain (peak 0.707) causes no clipping, letting the per-pixel
// adjustment commute with the area-average downscale (full-size vs preview).
ImageBufferPtr makeFullResGradient() {
    auto buf = ImageBuffer::create(8, 8, PixelFormat::F32);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const float v = 0.05f + 0.45f * static_cast<float>(x + y) / 14.0f;
            buf->setPixel(x, y, 0, v);
            buf->setPixel(x, y, 1, v * 0.8f);
            buf->setPixel(x, y, 2, v * 0.6f);
        }
    return buf;
}

} // namespace

TEST_CASE("Pipeline: maxDim=0 renders full-size buffers", "[pipeline][fullres]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    auto buf = makeFullResGradient();
    const Adjustments a = Adjustments::defaults();

    // render(..., 0), renderFull and renderLinearFull all return buffers that
    // match the input dimensions (full resolution, no downscale).
    auto out = pipeline.render(51, *buf, a, 0);
    REQUIRE(out != nullptr);
    REQUIRE(out->width() == buf->width());
    REQUIRE(out->height() == buf->height());
    REQUIRE(out->format() == PixelFormat::U8);

    auto full = pipeline.renderFull(52, *buf, a);
    REQUIRE(full != nullptr);
    REQUIRE(full->width() == buf->width());
    REQUIRE(full->height() == buf->height());
    REQUIRE(full->format() == PixelFormat::U8);

    auto linear = pipeline.renderLinearFull(53, *buf, a);
    REQUIRE(linear != nullptr);
    REQUIRE(linear->width() == buf->width());
    REQUIRE(linear->height() == buf->height());
    REQUIRE(linear->format() == PixelFormat::F32);

    // The 1:1 entry point: full-size display sRGB U8, not cached.
    auto display = pipeline.renderFullDisplay(54, *buf, a);
    REQUIRE(display != nullptr);
    REQUIRE(display->width() == buf->width());
    REQUIRE(display->height() == buf->height());
    REQUIRE(display->format() == PixelFormat::U8);
}

TEST_CASE("Pipeline: half-size preview matches area-averaged full-size render",
          "[pipeline][fullres]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    auto buf = makeFullResGradient();
    Adjustments a;
    a.exposure = 0.5; // +0.5 stop: peak 0.5 * 1.414 = 0.707 < 1 -> no clipping.

    // render(..., 4) downscales the *input* first, then adjusts, then converts
    // to display sRGB. renderLinearFull adjusts at native resolution; we then
    // area-average the linear result down to 4 and convert. For a pure linear
    // gain the two orders are equivalent, so the preview must equal the
    // downscaled full-size render.
    auto preview = pipeline.render(61, *buf, a, 4);
    auto fullLinear = pipeline.renderLinearFull(62, *buf, a);
    auto expected = cm->toDisplaySRGB(*fullLinear->resize(4, 4));

    REQUIRE(preview->width() == 4);
    REQUIRE(preview->height() == 4);
    REQUIRE(expected->width() == 4);
    REQUIRE(expected->height() == 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            for (int c = 0; c < 3; ++c)
                REQUIRE(preview->pixel(x, y, c) ==
                        Approx(expected->pixel(x, y, c)).margin(0.02f));
}

TEST_CASE("Pipeline: renderLinearFull and renderFullDisplay bypass the cache",
          "[pipeline][fullres]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    auto buf = makeFullResGradient();
    const Adjustments a = Adjustments::defaults();

    // Fill the LRU with a bounded preview first.
    pipeline.render(71, *buf, a, 4);
    const size_t before = pipeline.cacheBytes();
    REQUIRE(before > 0);

    // Full-resolution renders must never grow the preview LRU.
    pipeline.renderLinearFull(72, *buf, a);
    REQUIRE(pipeline.cacheBytes() == before);

    pipeline.renderFullDisplay(73, *buf, a);
    REQUIRE(pipeline.cacheBytes() == before);
}

TEST_CASE("Pipeline: render/renderFull with maxDim=0 still cache full-size",
          "[pipeline][fullres]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    auto buf = makeFullResGradient();
    const Adjustments a = Adjustments::defaults();

    REQUIRE(pipeline.cacheBytes() == 0);
    pipeline.render(81, *buf, a, 0);
    REQUIRE(pipeline.cacheBytes() > 0);
    const size_t afterRender = pipeline.cacheBytes();

    // renderFull is render(..., 0) under the hood, so it caches too. This
    // documents the current state: the 1:1 view must NOT go through these
    // entry points (it uses renderLinearFull + toDisplaySRGB / renderFullDisplay).
    pipeline.renderFull(82, *buf, a);
    REQUIRE(pipeline.cacheBytes() > afterRender);
}

TEST_CASE("Pipeline: default cache capacity matches Constants.kCacheCapacityMB",
          "[pipeline][cache]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    const size_t expected =
        static_cast<size_t>(lrc::app::kCacheCapacityMB) * 1024 * 1024;
    REQUIRE(pipeline.cacheCapacityBytes() == expected);
}

TEST_CASE("Pipeline: setCacheCapacity updates cacheCapacityBytes",
          "[pipeline][cache]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);

    pipeline.setCacheCapacity(1024 * 1024);
    REQUIRE(pipeline.cacheCapacityBytes() == 1024 * 1024);

    pipeline.setCacheCapacity(8 * 1024 * 1024);
    REQUIRE(pipeline.cacheCapacityBytes() == 8 * 1024 * 1024);
}

// --- Fast gaussian (box path) & neutral fast path ----------------------------

namespace {

// Reference exact separable gaussian: the O(radius) convolution Pipeline.cpp
// uses for sigma <= 4. Ground truth for validating the fast box path.
std::vector<float> exactGaussianRef(const float* src, int w, int h,
                                    float sigma) {
    std::vector<float> out(static_cast<size_t>(w) * h);
    const int radius = std::max(1, static_cast<int>(std::ceil(sigma * 3.0f)));
    std::vector<float> kernel(static_cast<size_t>(radius) * 2 + 1);
    float sum = 0.0f;
    for (int i = -radius; i <= radius; ++i) {
        const float v = std::exp(-0.5f * (static_cast<float>(i) / sigma) *
                                 (static_cast<float>(i) / sigma));
        kernel[static_cast<size_t>(i + radius)] = v;
        sum += v;
    }
    for (float& v : kernel) v /= sum;

    std::vector<float> tmp(out.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int i = -radius; i <= radius; ++i)
                acc += src[static_cast<size_t>(y) * w +
                           std::clamp(x + i, 0, w - 1)] *
                       kernel[static_cast<size_t>(i + radius)];
            tmp[static_cast<size_t>(y) * w + x] = acc;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int i = -radius; i <= radius; ++i)
                acc += tmp[static_cast<size_t>(std::clamp(y + i, 0, h - 1)) * w +
                           x] *
                       kernel[static_cast<size_t>(i + radius)];
            out[static_cast<size_t>(y) * w + x] = acc;
        }
    return out;
}

} // namespace

TEST_CASE("Pipeline: fast box blur approximates exact gaussian",
          "[pipeline][blur]") {
    const int w = 32, h = 32;
    // Smooth diagonal gradient spanning [0, 1].
    std::vector<float> grad(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            grad[static_cast<size_t>(y) * w + x] =
                static_cast<float>(x + y) / static_cast<float>(w + h - 2);

    // sigma = 8 > 4, so the production blur takes the 3-pass box path.
    const float sigma = 8.0f;
    const std::vector<float> box =
        lrc::pipelineGaussianBlurTest(grad.data(), w, h, sigma);
    const std::vector<float> exact = exactGaussianRef(grad.data(), w, h, sigma);

    double sumAbs = 0.0;
    for (size_t i = 0; i < box.size(); ++i)
        sumAbs += std::abs(static_cast<double>(box[i] - exact[i]));
    const double meanAbs = sumAbs / static_cast<double>(box.size());
    REQUIRE(meanAbs < 0.02);

    // The box path is a weighted average, so output stays bounded in [0,1].
    for (float v : box) {
        REQUIRE(v >= 0.0f);
        REQUIRE(v <= 1.0f);
    }
}

#if LRC_ENABLE_SIMD
// P2-2 acceptance (competitive-optimization spec §P2-2): the ALX2 exact-
// separable blur reassociates FMA arithmetic, so its output must stay within
// Δ<0.008 of the scalar reference rather than being bit-identical. This test
// only compiles when the engine is built with LRC_ENABLE_SIMD=ON (the default
// build uses the scalar path, which the byte-identity tests above already
// cover); LRC_ENABLE_SIMD is a PUBLIC compile definition propagated from
// lrc_engine, so the test TU sees it whenever the SIMD path is compiled in.
TEST_CASE("Pipeline: SIMD gaussianBlur is tolerance-close to scalar reference",
          "[pipeline][blur][simd]") {
    const int w = 160, h = 96;
    // Inline smooth diagonal gradient (same shape makeBlurGradient produces),
    // kept here so this SIMD test is self-contained regardless of ordering.
    std::vector<float> grad(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            grad[static_cast<size_t>(y) * w + x] =
                static_cast<float>(x + y) / static_cast<float>(w + h - 2);
    // A spread of exact-path sigmas: sub-pixel radius (0.8), a mid-width kernel
    // (2.5) and the exact/box boundary (4.0), exercising unlike tap counts.
    const float sigmas[] = {0.8f, 2.5f, 4.0f};
    for (const float sigma : sigmas) {
        const std::vector<float> simd =
            lrc::pipelineGaussianBlurTest(grad.data(), w, h, sigma);
        const std::vector<float> ref =
            exactGaussianRef(grad.data(), w, h, sigma);
        REQUIRE(simd.size() == ref.size());
        float maxDelta = 0.0f;
        for (size_t i = 0; i < simd.size(); ++i)
            maxDelta = std::max(maxDelta, std::abs(simd[i] - ref[i]));
        CAPTURE(sigma, maxDelta);
        REQUIRE(maxDelta < 0.008f);
    }
}
#endif // LRC_ENABLE_SIMD

// --- M1: pooled scratch buffers must not change the gaussian math ------------

namespace {

// Smooth diagonal gradient spanning [0,1] (same construction as the box-blur
// test above), used to exercise both gaussian paths deterministically.
std::vector<float> makeBlurGradient(int w, int h) {
    std::vector<float> grad(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            grad[static_cast<size_t>(y) * w + x] =
                static_cast<float>(x + y) / static_cast<float>(w + h - 2);
    return grad;
}

} // namespace

TEST_CASE("Pipeline: pooled gaussian blur is byte-identical across calls",
          "[pipeline][blur][m1]") {
    const int w = 320, h = 240;
    const std::vector<float> grad = makeBlurGradient(w, h);

    // Two paths: sigma=2.5 (exact separable convolution) and
    // sigma=min(w,h)/50 = 4.8 > 4 (3-pass sliding-window box cascade).
    const float sigmas[] = {2.5f, static_cast<float>(std::min(w, h)) / 50.0f};
    for (const float sigma : sigmas) {
        const std::vector<float> golden =
            lrc::pipelineGaussianBlurTest(grad.data(), w, h, sigma);
        // Interleave each re-render with a large scratch acquisition so the
        // thread-local pool must grow and reuse its buffers between calls.
        for (int rep = 0; rep < 6; ++rep) {
            ScratchBuffer big(static_cast<size_t>(w) * h * 2);
            std::fill(big.data(), big.data() + big.size(), 0.25f);
            const std::vector<float> again =
                lrc::pipelineGaussianBlurTest(grad.data(), w, h, sigma);
            REQUIRE(again == golden);  // byte-for-byte identical
        }
    }
}

TEST_CASE("Pipeline: scratch pool reuse does not corrupt blur output",
          "[pipeline][blur][m1]") {
    const int w = 64, h = 64;
    const std::vector<float> grad = makeBlurGradient(w, h);

    const std::vector<float> golden =
        lrc::pipelineGaussianBlurTest(grad.data(), w, h, 2.5f);

    // Grow the pool to a much larger buffer and fill it with a sentinel, then
    // render again: the blur temp must be fully rewritten, not stale.
    {
        ScratchBuffer big(static_cast<size_t>(w) * h * 4);
        std::fill(big.data(), big.data() + big.size(), 0.75f);
    }
    const std::vector<float> again =
        lrc::pipelineGaussianBlurTest(grad.data(), w, h, 2.5f);
    REQUIRE(again == golden);
}

TEST_CASE("Pipeline: texture+clarity+sharpening is deterministic with pooled scratch",
          "[pipeline][spatial][m1]") {
    const int w = 320, h = 240;
    std::vector<float> px(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < px.size(); ++i)
        px[i] = static_cast<float>((i * 1103515245u + 12345u) & 0xFFFFu) /
                static_cast<float>(0xFFFFu);
    const std::vector<float> input = px;

    Adjustments a;
    a.texture = 40.0;
    a.clarity = 30.0;
    a.sharpness = 60.0;
    a.sharpnessRadius = 2.0;
    a.sharpnessMasking = 25.0;
    a.sharpnessDetail = 50.0;

    // Render once to record golden output (texture+clarity share one luma,
    // sharpening recomputes its own; all buffers come from the scratch pool).
    Pipeline::applySpatialAdjustments(a, px.data(), w, h);
    const std::vector<float> golden = px;

    // Re-render on fresh identical inputs; the output must match byte-for-byte.
    std::vector<float> px2 = input;
    Pipeline::applySpatialAdjustments(a, px2.data(), w, h);
    REQUIRE(px2 == golden);

    std::vector<float> px3 = input;
    Pipeline::applySpatialAdjustments(a, px3.data(), w, h);
    REQUIRE(px3 == golden);
}

TEST_CASE("Pipeline: renderPreview with full adj stack is bit-exact across "
          "re-renders (ScratchBuffer hot path)",
          "[pipeline][scratch][S12]") {
    // S12: the gaussianBlur / lumaImage hot path uses thread-local ScratchBuffer
    // pools instead of per-call heap vectors. Two consecutive renderPreview
    // calls with identical inputs must produce byte-identical sRGB output,
    // proving the scratch path is deterministic (no stale-buffer corruption,
    // no per-frame heap allocation affecting the result).
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);

    // 256x192 F32 RGB gradient source.
    const int w = 256, h = 192;
    auto src = ImageBuffer::create(w, h, PixelFormat::F32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            src->setPixel(x, y, 0, static_cast<float>(x) / (w - 1));
            src->setPixel(x, y, 1, static_cast<float>(y) / (h - 1));
            src->setPixel(x, y, 2, 0.3f);
        }

    Adjustments a;
    a.exposure = 0.3;
    a.contrast = 12.0;
    a.texture = 35.0;
    a.clarity = 25.0;
    a.sharpness = 50.0;
    a.sharpnessRadius = 1.5;
    a.grain = 15.0;
    a.vignette = -10.0;

    const Size target{w, h};
    auto r1 = pipeline.renderPreview(*src, a, target, w, 1);
    REQUIRE(r1);
    // Second call: cache hit (no recompute) — same buffer object returned.
    auto r2 = pipeline.renderPreview(*src, a, target, w, 1);
    REQUIRE(r2.get() == r1.get());

    // Force a full recompute by changing imageId: scratch pool must produce
    // the exact same pixels as the first render.
    auto r3 = pipeline.renderPreview(*src, a, target, w, 99);
    REQUIRE(r3);
    REQUIRE(r3->sizeBytes() == r1->sizeBytes());
    REQUIRE(std::memcmp(r3->data().data(), r1->data().data(),
                         r1->sizeBytes()) == 0);
}

TEST_CASE("Pipeline: fully neutral adjustments are a pass-through",
          "[pipeline]") {
    // A spread of values (including extremes) across [0,1].
    const float values[] = {0.0f, 0.18f, 0.5f, 0.9f, 1.0f, 0.33f, 0.77f};
    std::vector<float> px;
    for (int i = 0; i < 64; ++i)
        for (int c = 0; c < 3; ++c)
            px.push_back(values[(i + c) % 7]);
    const std::vector<float> input = px;

    Pipeline::applyAdjustments(Adjustments::defaults(), px.data(),
                               px.size() / 3);
    REQUIRE(px.size() == input.size());
    for (size_t i = 0; i < px.size(); ++i)
        REQUIRE(px[i] == Approx(input[i]).margin(0.0));
}

TEST_CASE("Pipeline: split tone below 0.5 saturation is still a pass-through",
          "[pipeline][split]") {
    std::vector<float> px{0.2f, 0.4f, 0.6f, 0.8f, 0.5f, 0.3f};
    const std::vector<float> input = px;
    Adjustments a;
    a.splitTone.highlightSaturation = 0.4;
    a.splitTone.shadowSaturation = 0.4;
    Pipeline::applyAdjustments(a, px.data(), 2);
    for (size_t i = 0; i < px.size(); ++i)
        REQUIRE(px[i] == Approx(input[i]).margin(0.0));
}

// --- White-balance estimation (grey-world) ----------------------------------

TEST_CASE("Pipeline: estimateWhiteBalance is zero for neutral grey",
          "[pipeline][whitebalance]") {
    const auto e = Pipeline::estimateWhiteBalance(0.5f, 0.5f, 0.5f);
    REQUIRE(e.temperature == Approx(0.0).margin(1e-6));
    REQUIRE(e.tint == Approx(0.0).margin(1e-6));
}

TEST_CASE("Pipeline: estimateWhiteBalance inverts a known cast",
          "[pipeline][whitebalance]") {
    // Build a cast sample by applying the *inverse* pipeline white-balance
    // gains to a neutral 0.5 grey:
    //   lr = 0.5 / 2^(t0*0.006),  lb = 0.5 / 2^(-t0*0.006),
    //   lg = 0.5 / 2^(tint0*0.006)
    // The estimate must then recover exactly (t0, tint0).
    struct CastCase { double temperature; double tint; };
    const CastCase cases[] = {
        {40.0, 20.0},
        {-30.0, 50.0},
        {0.0, -25.0},
    };
    for (const CastCase& c : cases) {
        const float lr = static_cast<float>(0.5 / std::pow(2.0, c.temperature * 0.006));
        const float lb = static_cast<float>(0.5 / std::pow(2.0, -c.temperature * 0.006));
        const float lg = static_cast<float>(0.5 / std::pow(2.0, c.tint * 0.006));
        const auto e = Pipeline::estimateWhiteBalance(lr, lg, lb);
        REQUIRE(e.temperature == Approx(c.temperature).margin(1e-4));
        REQUIRE(e.tint == Approx(c.tint).margin(1e-4));
    }
}

TEST_CASE("Pipeline: estimateWhiteBalance is finite on degenerate inputs",
          "[pipeline][whitebalance]") {
    // All-zero sample: no ratios to compute, must return {0,0} with no NaN/Inf.
    const auto e0 = Pipeline::estimateWhiteBalance(0.0f, 0.0f, 0.0f);
    REQUIRE(e0.temperature == 0.0);
    REQUIRE(e0.tint == 0.0);
    REQUIRE(std::isfinite(e0.temperature));
    REQUIRE(std::isfinite(e0.tint));

    // A single dead channel is also degenerate.
    const auto er = Pipeline::estimateWhiteBalance(0.0f, 0.5f, 0.5f);
    REQUIRE(er.temperature == 0.0);
    REQUIRE(er.tint == 0.0);

    // Sub-threshold channels (< 1e-4) are treated as degenerate too.
    const auto es = Pipeline::estimateWhiteBalance(1e-6f, 0.5f, 0.5f);
    REQUIRE(es.temperature == 0.0);
    REQUIRE(es.tint == 0.0);
    REQUIRE(std::isfinite(es.temperature));
    REQUIRE(std::isfinite(es.tint));
}

TEST_CASE("Pipeline: estimateWhiteBalance sign points the right way",
          "[pipeline][whitebalance]") {
    // The pipeline's temperature > 0 warms (boosts R, cuts B). A reddish sample
    // (lr > lg ~= lb) must therefore be *cooled* (negative temperature) to
    // become neutral; a bluish sample warmed (positive temperature).
    const auto red = Pipeline::estimateWhiteBalance(0.6f, 0.5f, 0.5f);
    REQUIRE(red.temperature < 0.0);

    const auto blue = Pipeline::estimateWhiteBalance(0.5f, 0.5f, 0.6f);
    REQUIRE(blue.temperature > 0.0);

    // tint > 0 boosts G, so a greenish sample (lg > lr) needs negative tint;
    // a magenta-ish sample (lg below lr ~= lb) needs positive tint.
    const auto green = Pipeline::estimateWhiteBalance(0.5f, 0.6f, 0.5f);
    REQUIRE(green.tint < 0.0);

    const auto magenta = Pipeline::estimateWhiteBalance(0.5f, 0.4f, 0.5f);
    REQUIRE(magenta.tint > 0.0);
}

// --- M5 coarse-then-refine previews ------------------------------------------

namespace {

// 800x600 F32 linear gradient spanning [0, 1]. Longest side 800 exceeds
// kCoarseMaxDim (480), so a coarse render must downsample it.
ImageBufferPtr makeCoarseTestBuffer() {
    auto buf = ImageBuffer::create(800, 600, PixelFormat::F32);
    for (int y = 0; y < 600; ++y)
        for (int x = 0; x < 800; ++x) {
            const float v = static_cast<float>(x + y) / 1399.0f;
            buf->setPixel(x, y, 0, v);
            buf->setPixel(x, y, 1, v * 0.8f);
            buf->setPixel(x, y, 2, v * 0.6f);
        }
    return buf;
}

} // namespace

TEST_CASE("Pipeline: render at coarse maxDim downsamples to <= 480",
          "[pipeline][coarse]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    auto buf = makeCoarseTestBuffer();

    // 800x600 -> coarse (480): longest side 480, aspect ratio preserved.
    auto coarse = pipeline.render(91, *buf, Adjustments::defaults(),
                                  lrc::app::kCoarseMaxDim);
    REQUIRE(coarse != nullptr);
    REQUIRE(std::max(coarse->width(), coarse->height()) <=
            lrc::app::kCoarseMaxDim);
    REQUIRE(coarse->width() == 480);
    REQUIRE(coarse->height() == 360);

    // A small image (longest side 400 < 480) is never upscaled: render only
    // downsamples when the input exceeds maxDim.
    auto small = ImageBuffer::create(400, 300, PixelFormat::F32);
    for (int y = 0; y < 300; ++y)
        for (int x = 0; x < 400; ++x) {
            const float v = static_cast<float>(x + y) / 699.0f;
            small->setPixel(x, y, 0, v);
            small->setPixel(x, y, 1, v);
            small->setPixel(x, y, 2, v);
        }
    auto smallOut = pipeline.render(92, *small, Adjustments::defaults(),
                                    lrc::app::kCoarseMaxDim);
    REQUIRE(smallOut != nullptr);
    REQUIRE(std::max(smallOut->width(), smallOut->height()) <= 400);
    REQUIRE(smallOut->width() == 400);
    REQUIRE(smallOut->height() == 300);
}

TEST_CASE("Pipeline: coarse and full cache keys are independent",
          "[pipeline][coarse]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    auto buf = makeCoarseTestBuffer();
    const Adjustments a = Adjustments::defaults();

    // Same imageId + same adjustments; only maxDim differs (480 vs full).
    auto coarse = pipeline.render(101, *buf, a, lrc::app::kCoarseMaxDim);
    auto full = pipeline.render(101, *buf, a, 0);
    REQUIRE(coarse != nullptr);
    REQUIRE(full != nullptr);
    REQUIRE(coarse->width() < full->width());
    REQUIRE(coarse->height() < full->height());

    // Re-rendering either resolution must hit its own cache slot, not be
    // polluted by the other (pointer equality = served from the LRU).
    auto coarseAgain = pipeline.render(101, *buf, a, lrc::app::kCoarseMaxDim);
    REQUIRE(coarseAgain->width() == coarse->width());
    REQUIRE(coarseAgain->height() == coarse->height());
    REQUIRE(coarseAgain->data().data() == coarse->data().data());

    auto fullAgain = pipeline.render(101, *buf, a, 0);
    REQUIRE(fullAgain->width() == full->width());
    REQUIRE(fullAgain->height() == full->height());
    REQUIRE(fullAgain->data().data() == full->data().data());
}

// --- M16: pixel-effect coverage for stages with no rendering tests ----------
// (tone curve / contrast / dehaze / vibrance / distortion / vignette)

TEST_CASE("Pipeline: contrast enhances the tonal difference around mid-gray",
          "[pipeline][m16]") {
    // dark, mid-gray, bright
    std::vector<float> px{0.05f, 0.05f, 0.05f,
                          0.18f, 0.18f, 0.18f,
                          0.80f, 0.80f, 0.80f};
    const auto input = px;
    Adjustments a;
    a.contrast = 100.0;
    Pipeline::applyAdjustments(a, px.data(), 3);

    // Mid-gray is the pivot point: it must stay put.
    REQUIRE(px[3] == Approx(0.18f).margin(0.01f));
    // Dark values are pushed down, bright values pushed up.
    REQUIRE(px[0] < input[0]);
    REQUIRE(px[6] > input[6]);
    // The contrast change is non-trivial.
    REQUIRE(input[0] - px[0] > 0.02f);
    REQUIRE(px[6] - input[6] > 0.02f);
}

TEST_CASE("Pipeline: S-curve darkens shadows and lifts highlights",
          "[pipeline][m16]") {
    std::vector<float> px{0.10f, 0.10f, 0.10f,
                          0.50f, 0.50f, 0.50f,
                          0.90f, 0.90f, 0.90f};
    const auto input = px;
    Adjustments a;
    a.curve = {{0.0f, 0.0f}, {0.25f, 0.20f}, {0.50f, 0.50f},
               {0.75f, 0.80f}, {1.0f, 1.0f}};
    Pipeline::applyAdjustments(a, px.data(), 3);

    // Shadows sit below the identity diagonal -> darkened.
    REQUIRE(px[0] < input[0]);
    // Highlights sit above the identity diagonal -> lifted.
    REQUIRE(px[6] > input[6]);
    // The mid point is exactly on the curve.
    REQUIRE(px[3] == Approx(0.5f).margin(0.02f));
    REQUIRE(input[0] - px[0] > 0.01f);
    REQUIRE(px[6] - input[6] > 0.01f);
}

TEST_CASE("Pipeline: dehaze spreads the tonal range around the haze floor",
          "[pipeline][m16]") {
    std::vector<float> px{0.10f, 0.10f, 0.10f, 0.50f, 0.50f, 0.50f};
    const auto input = px;
    Adjustments a;
    a.dehaze = 50.0;
    Pipeline::applyAdjustments(a, px.data(), 2);

    // Both pixels move: non-zero effect.
    REQUIRE(std::abs(px[0] - input[0]) > 0.01f);
    REQUIRE(std::abs(px[3] - input[3]) > 0.01f);
    // The bright/dark gap widens -> contrast increases.
    const float inGap = input[3] - input[0];
    const float outGap = px[3] - px[0];
    REQUIRE(outGap > inGap);
}

TEST_CASE("Pipeline: vibrance boosts muted colours and protects skin tones",
          "[pipeline][m16]") {
    // Muted orange (skin hue ~30 deg) and its non-skin mirror (blue, hue 210).
    std::vector<float> orange{0.4f, 0.35f, 0.3f};
    std::vector<float> blue{0.3f, 0.35f, 0.4f};
    Adjustments a;
    a.vibrance = 100.0;

    auto spread = [](const std::vector<float>& v) {
        return std::max({v[0], v[1], v[2]}) - std::min({v[0], v[1], v[2]});
    };
    const float orangeIn = spread(orange);
    const float blueIn = spread(blue);

    Pipeline::applyAdjustments(a, orange.data(), 1);
    Pipeline::applyAdjustments(a, blue.data(), 1);

    const float orangeOut = spread(orange);
    const float blueOut = spread(blue);

    // Muted colours gain saturation.
    REQUIRE(orangeOut > orangeIn);
    REQUIRE(blueOut > blueIn);
    // The skin-tone (orange) hue receives a smaller boost than the blue hue.
    REQUIRE(blueOut > orangeOut);
}

TEST_CASE("Pipeline: positive distortion pulls pixels outward from the centre",
          "[pipeline][m16]") {
    const int w = 65, h = 65;
    std::vector<float> px(static_cast<size_t>(w) * h * 3);
    // Horizontal linear gradient v = x/(w-1), identical in all channels.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float v = static_cast<float>(x) / static_cast<float>(w - 1);
            const size_t i = (static_cast<size_t>(y) * w + x) * 3;
            px[i] = px[i + 1] = px[i + 2] = v;
        }
    const auto input = px;
    Adjustments a;
    a.distortion = 100.0;
    Pipeline::applySpatialAdjustments(a, px.data(), w, h);

    const int cx = w / 2;
    auto at = [&](int x, int y) {
        return px[(static_cast<size_t>(y) * w + x) * 3];
    };
    auto inAt = [&](int x, int y) {
        return input[(static_cast<size_t>(y) * w + x) * 3];
    };

    // The remap axis is the centre pixel: unchanged.
    REQUIRE(at(cx, cx) == Approx(inAt(cx, cx)).margin(1e-4f));
    // Right of centre the source is sampled from further right -> brighter.
    REQUIRE(at(56, cx) > inAt(56, cx));
    // Left of centre the source is sampled from further left -> darker.
    REQUIRE(at(8, cx) < inAt(8, cx));
    // Non-zero total change across the buffer.
    float sumAbs = 0.0f;
    for (size_t i = 0; i < px.size(); i += 3)
        sumAbs += std::abs(px[i] - input[i]);
    REQUIRE(sumAbs > 0.01f);
}

TEST_CASE("Pipeline: vignette shades corners relative to the centre",
          "[pipeline][m16]") {
    const int w = 65, h = 65;
    auto makeGray = [&](float v) {
        return std::vector<float>(static_cast<size_t>(w) * h * 3, v);
    };
    auto at = [&](const std::vector<float>& buf, int x, int y) {
        return buf[(static_cast<size_t>(y) * w + x) * 3];
    };

    // Negative vignette darkens the corners.
    std::vector<float> neg = makeGray(0.5f);
    Adjustments a;
    a.vignette = -100.0;
    Pipeline::applySpatialAdjustments(a, neg.data(), w, h);
    REQUIRE(at(neg, 32, 32) == Approx(0.5f).margin(1e-4f));
    REQUIRE(at(neg, 0, 0) < 0.1f);

    // Positive vignette brightens the corners.
    std::vector<float> pos = makeGray(0.5f);
    Adjustments b;
    b.vignette = 100.0;
    Pipeline::applySpatialAdjustments(b, pos.data(), w, h);
    REQUIRE(at(pos, 32, 32) == Approx(0.5f).margin(1e-4f));
    REQUIRE(at(pos, 0, 0) > 0.9f);
}

TEST_CASE("Pipeline: every M16 stage changes the rendered output",
          "[pipeline][m16]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    auto buf = ImageBuffer::create(40, 40, PixelFormat::F32);
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x) {
            const float v = static_cast<float>(x + y) / 78.0f;
            buf->setPixel(x, y, 0, v);
            buf->setPixel(x, y, 1, v * 0.7f);
            buf->setPixel(x, y, 2, v * 0.4f);
        }
    const auto base = pipeline.render(2000, *buf, Adjustments::defaults(), 20);
    const auto differs = [&](const Adjustments& a) {
        const auto out = pipeline.render(2000, *buf, a, 20);
        if (out->sizeBytes() != base->sizeBytes())
            return true;
        return std::memcmp(out->data().data(), base->data().data(),
                           out->sizeBytes()) != 0;
    };

    Adjustments curve;
    curve.curve = {{0.0f, 0.0f}, {0.25f, 0.20f}, {0.50f, 0.50f},
                   {0.75f, 0.80f}, {1.0f, 1.0f}};
    REQUIRE(differs(curve));

    Adjustments contrast;
    contrast.contrast = 50.0;
    REQUIRE(differs(contrast));

    Adjustments dehaze;
    dehaze.dehaze = 50.0;
    REQUIRE(differs(dehaze));

    Adjustments vibrance;
    vibrance.vibrance = 60.0;
    REQUIRE(differs(vibrance));

    Adjustments distortion;
    distortion.distortion = 80.0;
    REQUIRE(differs(distortion));

    Adjustments vignette;
    vignette.vignette = -80.0;
    REQUIRE(differs(vignette));

    // Noise reduction: both luma NR and chroma NR must change the rendered
    // pixels (not be silent no-ops in the spatial stage).
    Adjustments nr;
    nr.luminance = 80.0;
    REQUIRE(differs(nr));

    Adjustments chroma;
    chroma.colorNoiseReduction = 80.0;
    REQUIRE(differs(chroma));
}

// --- M18: texture / clarity / sharpening must actually change pixels --------
// The earlier "three identical re-renders" tests would pass even if these
// stages were a no-op; these assert real, direction-correct pixel effects.

namespace {

// w*h grayscale image with a horizontal edge: y < h/2 -> lo, y >= h/2 -> hi.
std::vector<float> makeEdgeImage(int w, int h, float lo, float hi) {
    std::vector<float> px(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float v = (y < h / 2) ? lo : hi;
            const size_t i = (static_cast<size_t>(y) * w + x) * 3;
            px[i] = px[i + 1] = px[i + 2] = v;
        }
    return px;
}

// Hermite smoothstep, mirroring Pipeline's file-local helper (must stay
// identical so the reference below reproduces the pre-M4 clarity path).
inline float step01(float x) {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

// Full-resolution clarity reference (the pre-M4-1 path): a single-channel
// gaussian blur at full resolution via the test hook, then the high-frequency
// detail added with the same protect/gain logic. Used as the tolerance baseline
// for the M4-1 downsample-blur optimisation.
std::vector<float> refClarityFullRes(const std::vector<float>& rgb, int w,
                                     int h, double clarity) {
    const size_t n = static_cast<size_t>(w) * h;
    std::vector<float> luma(n);
    for (size_t i = 0; i < n; ++i)
        luma[i] = 0.2126f * rgb[i * 3] + 0.7152f * rgb[i * 3 + 1] +
                  0.0722f * rgb[i * 3 + 2];
    const float radius =
        std::max(4.0f, static_cast<float>(std::min(w, h)) / 50.0f);
    const float gain = static_cast<float>(clarity) / 100.0f * 0.45f;
    const std::vector<float> blurred =
        pipelineGaussianBlurTest(luma.data(), w, h, radius);
    std::vector<float> out = rgb;
    for (size_t i = 0; i < n; ++i) {
        const float L = luma[i];
        const float protect = step01(L / 0.05f) *
                              (1.0f - step01((L - 0.92f) / 0.08f));
        const float d = (L - blurred[i]) * gain * protect;
        out[i * 3] += d;
        out[i * 3 + 1] += d;
        out[i * 3 + 2] += d;
    }
    return out;
}

} // namespace

TEST_CASE("Pipeline: texture increases local contrast across an edge",
          "[pipeline][spatial][m18]") {
    const int w = 64, h = 64;
    auto px = makeEdgeImage(w, h, 0.2f, 0.8f);
    const auto input = px;
    Adjustments a;
    a.texture = 100.0;
    Pipeline::applySpatialAdjustments(a, px.data(), w, h);

    auto at = [&](int x, int y) {
        return px[(static_cast<size_t>(y) * w + x) * 3];
    };
    auto inAt = [&](int x, int y) {
        return input[(static_cast<size_t>(y) * w + x) * 3];
    };

    // Just below the edge: brightened; just above: darkened.
    REQUIRE(at(32, 33) > inAt(32, 33));
    REQUIRE(at(32, 31) < inAt(32, 31));
    // Far from the edge the flat region is unchanged (high-pass kills DC).
    REQUIRE(at(32, 0) == Approx(0.2f).margin(0.01f));
    REQUIRE(at(32, 63) == Approx(0.8f).margin(0.01f));
    // And the change is non-trivial.
    REQUIRE(at(32, 33) - inAt(32, 33) > 0.005f);
    REQUIRE(inAt(32, 31) - at(32, 31) > 0.005f);
}

TEST_CASE("Pipeline: clarity increases local contrast across an edge",
          "[pipeline][spatial][m18]") {
    const int w = 64, h = 64;
    auto px = makeEdgeImage(w, h, 0.2f, 0.8f);
    const auto input = px;
    Adjustments a;
    a.clarity = 100.0;
    Pipeline::applySpatialAdjustments(a, px.data(), w, h);

    auto at = [&](int x, int y) {
        return px[(static_cast<size_t>(y) * w + x) * 3];
    };
    auto inAt = [&](int x, int y) {
        return input[(static_cast<size_t>(y) * w + x) * 3];
    };

    REQUIRE(at(32, 33) > inAt(32, 33));
    REQUIRE(at(32, 31) < inAt(32, 31));
    REQUIRE(at(32, 0) == Approx(0.2f).margin(0.01f));
    REQUIRE(at(32, 63) == Approx(0.8f).margin(0.01f));
    REQUIRE(at(32, 33) - inAt(32, 33) > 0.005f);
    REQUIRE(inAt(32, 31) - at(32, 31) > 0.005f);
}

TEST_CASE("Pipeline: M4-1 clarity downsample blur within tolerance of full-res",
          "[pipeline][spatial][m4]") {
    // The M4-1 clarity path blurs a ~1/4-linear-size luma downsample instead of
    // the full-res luma. It must stay visually close to the old full-res 3-box
    // result: max per-channel diff < 0.008 or PSNR > 40 dB (M4 plan).
    const int w = 256, h = 192;
    auto px = makeEdgeImage(w, h, 0.2f, 0.8f);
    // Blend in a horizontal gradient so the downsample has real low-frequency
    // content rather than only a hard edge.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float edge = (y < h / 2) ? 0.2f : 0.8f;
            const float grad = 0.2f + 0.6f * static_cast<float>(x) / w;
            const float v = 0.5f * edge + 0.5f * grad;
            const size_t i = (static_cast<size_t>(y) * w + x) * 3;
            px[i] = px[i + 1] = px[i + 2] = v;
        }
    Adjustments a;
    a.clarity = 100.0;
    auto out = px;
    Pipeline::applySpatialAdjustments(a, out.data(), w, h);
    const std::vector<float> ref = refClarityFullRes(px, w, h, 100.0);

    float maxDiff = 0.0f;
    double mse = 0.0;
    const size_t total = static_cast<size_t>(w) * h * 3;
    for (size_t i = 0; i < total; ++i) {
        const float d = std::fabs(out[i] - ref[i]);
        maxDiff = std::max(maxDiff, d);
        mse += static_cast<double>(d) * d;
    }
    mse /= static_cast<double>(total);
    const double psnr =
        (mse > 0.0) ? 20.0 * std::log10(1.0 / std::sqrt(mse)) : 999.0;
    INFO("clarity downsample maxDiff=" << maxDiff << " PSNR=" << psnr);
    REQUIRE((maxDiff < 0.008f || psnr > 40.0));
}

TEST_CASE("Pipeline: sharpening enhances edge contrast",
          "[pipeline][spatial][m18]") {
    const int w = 64, h = 64;
    auto px = makeEdgeImage(w, h, 0.3f, 0.7f);
    const auto input = px;
    Adjustments a;
    a.sharpness = 60.0;
    a.sharpnessRadius = 1.0;
    a.sharpnessDetail = 0.0;
    a.sharpnessMasking = 0.0;
    Pipeline::applySpatialAdjustments(a, px.data(), w, h);

    auto at = [&](int x, int y) {
        return px[(static_cast<size_t>(y) * w + x) * 3];
    };
    auto inAt = [&](int x, int y) {
        return input[(static_cast<size_t>(y) * w + x) * 3];
    };

    // The bright side of the edge gets brighter, the dark side darker.
    REQUIRE(at(32, 32) > inAt(32, 32));
    REQUIRE(at(32, 31) < inAt(32, 31));
    // Flat regions far from the edge are untouched.
    REQUIRE(at(32, 0) == Approx(0.3f).margin(0.01f));
    REQUIRE(at(32, 63) == Approx(0.7f).margin(0.01f));
    REQUIRE(at(32, 32) - inAt(32, 32) > 0.005f);
    REQUIRE(inAt(32, 31) - at(32, 31) > 0.005f);
}

// --- Manual noise reduction (luma / chroma NR) -------------------------------
// The NR block in Pipeline::applySpatialAdjustments splits RGB into BT.709
// YCbCr, denoises Cb/Cr with a gaussian blend and Y with an edge-preserving
// local-variance attenuator, then reconstructs. These tests use
// renderLinearFull (F32, full resolution): a downscaled U8 preview would
// area-average the injected noise away and quantize the residual, masking the
// variance reduction under test.

namespace {

// Deterministic flat-region noise in [-amp, amp]. A sum of two incommensurate
// sinusoids is aperiodic over a 200px integer lattice, so it behaves like
// pseudo-random noise while being fully reproducible.
inline float nrNoise(int x, int y, float amp) {
    return amp * 0.5f *
           (std::sin(static_cast<float>(x) * 7.3f +
                     static_cast<float>(y) * 3.1f) +
            std::sin(static_cast<float>(x) * 13.7f +
                     static_cast<float>(y) * 5.9f));
}

// Grayscale F32 buffer: 0.5 base + nrNoise everywhere, plus a sharp vertical
// step of `edge` for every column x >= w/2. All three channels are equal, so
// the Rec.709 luma equals the stored channel value.
ImageBufferPtr makeNrLumaBuffer(int w, int h, float noiseAmp, float edge) {
    auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
    const int mid = w / 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float v = 0.5f + nrNoise(x, y, noiseAmp) +
                            ((x >= mid) ? edge : 0.0f);
            buf->setPixel(x, y, 0, v);
            buf->setPixel(x, y, 1, v);
            buf->setPixel(x, y, 2, v);
        }
    return buf;
}

// Neutral 0.5 grey with green-only noise (r == b == 0.5, g wobbles around 0.5):
// per-pixel colour separation (chroma noise) with no luma variation.
ImageBufferPtr makeNrChromaBuffer(int w, int h, float noiseAmp) {
    auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float g = 0.5f + nrNoise(x, y, noiseAmp);
            buf->setPixel(x, y, 0, 0.5f);
            buf->setPixel(x, y, 1, g);
            buf->setPixel(x, y, 2, 0.5f);
        }
    return buf;
}

// Rec.709 luma of the pixel at (x, y) in an F32 buffer.
inline float nrBufLuma(const ImageBuffer& buf, int x, int y) {
    return 0.2126f * buf.pixel(x, y, 0) + 0.7152f * buf.pixel(x, y, 1) +
           0.0722f * buf.pixel(x, y, 2);
}

// Sample standard deviation of luma over the box [x0,x1) x [y0,y1).
double nrRegionLumaStd(const ImageBuffer& buf, int x0, int x1, int y0,
                       int y1) {
    double sum = 0.0, sum2 = 0.0;
    size_t n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const double v = nrBufLuma(buf, x, y);
            sum += v;
            sum2 += v * v;
            ++n;
        }
    if (n == 0)
        return 0.0;
    const double mean = sum / static_cast<double>(n);
    const double var = std::max(0.0, sum2 / static_cast<double>(n) - mean * mean);
    return std::sqrt(var);
}

// Mean of (|r-g| + |g-b|) over the box [x0,x1) x [y0,y1). This colour-difference
// proxy is 0 for neutral grey and grows with chroma noise.
double nrRegionColourDiff(const ImageBuffer& buf, int x0, int x1, int y0,
                          int y1) {
    double sum = 0.0;
    size_t n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            sum += std::abs(buf.pixel(x, y, 0) - buf.pixel(x, y, 1)) +
                   std::abs(buf.pixel(x, y, 1) - buf.pixel(x, y, 2));
            ++n;
        }
    return (n == 0) ? 0.0 : sum / static_cast<double>(n);
}

} // namespace

TEST_CASE("Pipeline: luminance NR reduces flat-region noise and keeps edges",
          "[pipeline][spatial][nr]") {
    const int w = 200, h = 200;
    const int mid = w / 2;
    auto buf = makeNrLumaBuffer(w, h, 0.02f, 0.2f);

    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    Adjustments def;
    Adjustments nr;
    nr.luminance = 100.0;

    const auto base = pipeline.renderLinearFull(3001, *buf, def);
    const auto denoised = pipeline.renderLinearFull(3002, *buf, nr);

    // Flat sample boxes sit 8 px clear of the step edge and the buffer border,
    // so neither edge bleed nor edge clamping can skew the variance. The left
    // (0.5) and right (0.7) plateaus are measured independently.
    const int x0 = 8, x1 = mid - 8;
    const int xr0 = mid + 8, xr1 = w - 8;
    const int y0 = 8, y1 = h - 8;
    const double stdBaseL = nrRegionLumaStd(*base, x0, x1, y0, y1);
    const double stdNRL = nrRegionLumaStd(*denoised, x0, x1, y0, y1);
    const double stdBaseR = nrRegionLumaStd(*base, xr0, xr1, y0, y1);
    const double stdNRR = nrRegionLumaStd(*denoised, xr0, xr1, y0, y1);

    // Sanity: the injected noise must dominate the flat-region std.
    REQUIRE(stdBaseL > 0.004);
    REQUIRE(stdBaseR > 0.004);
    // Luminance NR must cut the flat-region variance by more than 20%.
    REQUIRE(stdNRL < 0.8 * stdBaseL);
    REQUIRE(stdNRR < 0.8 * stdBaseR);

    // The step edge survives: the luma gap across x == mid stays large.
    double gapBase = 0.0, gapNR = 0.0;
    size_t nRows = 0;
    for (int y = y0; y < y1; ++y) {
        gapBase += std::abs(nrBufLuma(*base, mid, y) -
                            nrBufLuma(*base, mid - 1, y));
        gapNR += std::abs(nrBufLuma(*denoised, mid, y) -
                          nrBufLuma(*denoised, mid - 1, y));
        ++nRows;
    }
    gapBase /= static_cast<double>(nRows);
    gapNR /= static_cast<double>(nRows);
    REQUIRE(gapBase > 0.15);
    REQUIRE(gapNR > 0.5 * gapBase);
}

TEST_CASE("Pipeline: chroma NR reduces colour noise without shifting luma",
          "[pipeline][spatial][nr]") {
    const int w = 200, h = 200;
    auto buf = makeNrChromaBuffer(w, h, 0.03f);

    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    Adjustments def;
    Adjustments nr;
    nr.colorNoiseReduction = 100.0;

    const auto base = pipeline.renderLinearFull(3011, *buf, def);
    const auto denoised = pipeline.renderLinearFull(3012, *buf, nr);

    const int x0 = 8, x1 = w - 8, y0 = 8, y1 = h - 8;
    const double diffBase = nrRegionColourDiff(*base, x0, x1, y0, y1);
    const double diffNR = nrRegionColourDiff(*denoised, x0, x1, y0, y1);

    // The green-only wobble must produce a measurable colour difference...
    REQUIRE(diffBase > 0.01);
    // ...and chroma NR must cut it by more than 20%.
    REQUIRE(diffNR < 0.8 * diffBase);

    // Mean luma is preserved: chroma NR must not darken or brighten the image.
    double sumBase = 0.0, sumNR = 0.0;
    size_t n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            sumBase += nrBufLuma(*base, x, y);
            sumNR += nrBufLuma(*denoised, x, y);
            ++n;
        }
    const double meanBase = sumBase / static_cast<double>(n);
    const double meanNR = sumNR / static_cast<double>(n);
    REQUIRE(std::abs(meanNR - meanBase) < 0.02);
}

TEST_CASE("Pipeline: neutral NR settings are a byte-identical pass-through",
          "[pipeline][spatial][nr]") {
    // A non-grey, non-flat image: if the NR block ran on neutral settings, the
    // YCbCr round-trip would leave a floating-point footprint in the F32 bytes.
    // The neutral fast path must skip the block and reproduce the default
    // render byte-for-byte. renderLinearFull is uncached, so both renders
    // actually execute (a cached render would compare a buffer with itself).
    auto buf = ImageBuffer::create(64, 64, PixelFormat::F32);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const float v = static_cast<float>(x + y) / 126.0f;
            buf->setPixel(x, y, 0, v);
            buf->setPixel(x, y, 1, v * 0.8f);
            buf->setPixel(x, y, 2, v * 0.6f);
        }

    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    Adjustments def;
    Adjustments nrNeutral; // luminance = 0, colorNoiseReduction = 0 (defaults)
    nrNeutral.luminance = 0.0;
    nrNeutral.colorNoiseReduction = 0.0;

    const auto base = pipeline.renderLinearFull(3021, *buf, def);
    const auto neutral = pipeline.renderLinearFull(3022, *buf, nrNeutral);

    REQUIRE(base->format() == PixelFormat::F32);
    REQUIRE(neutral->format() == PixelFormat::F32);
    REQUIRE(base->sizeBytes() == neutral->sizeBytes());
    REQUIRE(std::memcmp(base->data().data(), neutral->data().data(),
                        base->sizeBytes()) == 0);
    // The default render is itself a pure pass-through of the input.
    REQUIRE(std::memcmp(base->data().data(), buf->data().data(),
                        base->sizeBytes()) == 0);
}

// --- S5/S6: assertions that document the fixed behaviour --------------------
// These depend on source fixes by parallel group B; they FAIL against the
// pre-fix Pipeline.cpp and must not be weakened for the acceptance build.

TEST_CASE("Pipeline: sharpening with masking stays active on strong edges (S6)",
          "[pipeline][spatial][m18][s6]") {
    const int w = 64, h = 64;
    // Vertical edge: x < 32 dark (0.3), x >= 32 bright (0.7).
    std::vector<float> px(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float v = (x < 32) ? 0.3f : 0.7f;
            const size_t i = (static_cast<size_t>(y) * w + x) * 3;
            px[i] = px[i + 1] = px[i + 2] = v;
        }
    const auto input = px;
    Adjustments a;
    a.sharpness = 80.0;
    a.sharpnessRadius = 1.0;
    a.sharpnessDetail = 50.0;
    a.sharpnessMasking = 25.0;  // inside the (10,50] dead zone that S6 must fix.
    Pipeline::applySpatialAdjustments(a, px.data(), w, h);

    auto at = [&](int x, int y) {
        return px[(static_cast<size_t>(y) * w + x) * 3];
    };
    auto inAt = [&](int x, int y) {
        return input[(static_cast<size_t>(y) * w + x) * 3];
    };
    // Documented correct behaviour: masking > 0 sharpens only strong edges,
    // continuously (no dead zone). A strong step edge must still overshoot.
    REQUIRE(at(32, 32) > inAt(32, 32));
    REQUIRE(at(31, 32) < inAt(31, 32));
}

TEST_CASE("Pipeline: global saturation stacks with per-channel HSL (S5)",
          "[pipeline][hsl][s5]") {
    const float r0 = 0.7f, g0 = 0.3f, b0 = 0.2f;

    Adjustments ch;
    ch.hsl[static_cast<size_t>(ColorRange::Red)].saturation = 50.0;
    auto outCh = std::vector<float>{r0, g0, b0};
    Pipeline::applyAdjustments(ch, outCh.data(), 1);

    Adjustments both;
    both.saturation = 50.0;
    both.hsl[static_cast<size_t>(ColorRange::Red)].saturation = 50.0;
    auto outBoth = std::vector<float>{r0, g0, b0};
    Pipeline::applyAdjustments(both, outBoth.data(), 1);

    auto spread = [](const std::vector<float>& v) {
        return std::max({v[0], v[1], v[2]}) - std::min({v[0], v[1], v[2]});
    };
    // Documented correct behaviour: the global Saturation slider and the
    // per-channel HSL stack must both apply (they compose). S5 fixes the
    // current code silently dropping the global slider whenever any HSL
    // channel is active.
    REQUIRE(spread(outBoth) > spread(outCh));
}
