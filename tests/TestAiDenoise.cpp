// PHASE_12 — AI denoise.
//
// Coverage:
//   1. Strength-blending curve: strength==0 -> output identical to input
//      (AiDenoiseProcessor::blend enforces this).
//   2. The bilateral fallback path produces a less noisy image for a
//      visibly-noisy synthetic input — measured by a simple local
//      horizontal-difference metric.
//   3. A variety of image sizes (tiny, tile-sized, larger-than-tile) all
//      produce a valid F32 buffer with matching dimensions.
//   4. AiDenoise::hasEffect reflects strength + enabled.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>

#include "engine/Adjustments.h"
#include "engine/AiDenoiseProcessor.h"
#include "engine/ImageBuffer.h"

namespace lrc::tests {

static ImageBuffer makeNoisyBuffer(int w, int h, uint32_t seed) {
    ImageBuffer buf(w, h, PixelFormat::F32);
    std::mt19937 rng(seed);
    std::normal_distribution<float> n(0.0f, 0.08f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                const float base = 0.3f + 0.2f * sinf(x * 0.12f)
                                        + 0.2f * cosf(y * 0.09f + c);
                buf.setPixel(x, y, c,
                             std::clamp(base + n(rng), 0.0f, 1.0f));
            }
        }
    }
    return buf;
}

static double meanAbsoluteDifference(const ImageBuffer& a, const ImageBuffer& b) {
    REQUIRE(a.width() == b.width());
    REQUIRE(a.height() == b.height());
    REQUIRE(a.format() == b.format());
    const float* fa = reinterpret_cast<const float*>(a.data().data());
    const float* fb = reinterpret_cast<const float*>(b.data().data());
    double s = 0.0;
    const int total = a.width() * a.height() * 3;
    for (int i = 0; i < total; ++i)
        s += std::fabs(static_cast<double>(fa[i] - fb[i]));
    return s / static_cast<double>(std::max(1, total));
}

static ImageBuffer cloneBuffer(const ImageBuffer& in) {
    // ImageBuffer has no built-in clone(); build a bytewise copy via its
    // underlying data vector (trivially safe for the interleaved POD layout).
    ImageBuffer out(in.width(), in.height(), in.format());
    out.data() = in.data();
    return out;
}

TEST_CASE("AiDenoise blend: strength=0 returns A exactly",
          "[ai][denoise][blend]") {
    ImageBuffer a(16, 8, PixelFormat::F32);
    ImageBuffer b(16, 8, PixelFormat::F32);
    for (int i = 0; i < 16 * 8 * 3; ++i) {
        reinterpret_cast<float*>(a.data().data())[i] = 0.3f;
        reinterpret_cast<float*>(b.data().data())[i] = 0.9f;
    }
    auto mixed = AiDenoiseProcessor::blend(a, b, 0.0f);
    REQUIRE(mixed);
    const double d = meanAbsoluteDifference(a, *mixed);
    CHECK(d == 0.0);
}

TEST_CASE("AiDenoise blend: strength=1 returns B exactly",
          "[ai][denoise][blend]") {
    ImageBuffer a(16, 8, PixelFormat::F32);
    ImageBuffer b(16, 8, PixelFormat::F32);
    for (int i = 0; i < 16 * 8 * 3; ++i) {
        reinterpret_cast<float*>(a.data().data())[i] = 0.1f;
        reinterpret_cast<float*>(b.data().data())[i] = 0.7f;
    }
    auto mixed = AiDenoiseProcessor::blend(a, b, 1.0f);
    REQUIRE(mixed);
    const double d = meanAbsoluteDifference(b, *mixed);
    CHECK(d == 0.0);
}

TEST_CASE("AiDenoiseProcessor: fallback bilateral reduces noise",
          "[ai][denoise][bilateral]") {
    auto noisy = makeNoisyBuffer(64, 48, 0xA5A5);
    AiDenoiseOptions opts;
    opts.strength = 1.0f;
    opts.luminanceOnly = false;

    AiDenoiseProcessor proc(nullptr);
    // Force fallback by passing an empty model id.
    auto denoised = proc.denoise(noisy, "", opts);
    REQUIRE(denoised);
    CHECK(denoised->width() == noisy.width());
    CHECK(denoised->height() == noisy.height());

    const double noisyVsDenoised = meanAbsoluteDifference(noisy, *denoised);
    CHECK(noisyVsDenoised > 0.005);

    // Roughness = average |pixel - pixel-right| across all channels — a
    // denoiser should strictly reduce this local-variability metric.
    auto roughness = [](const ImageBuffer& b) {
        double s = 0.0;
        int n = 0;
        for (int y = 0; y < b.height(); ++y) {
            for (int x = 0; x < b.width() - 1; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const double p = b.pixel(x, y, c);
                    const double q = b.pixel(x + 1, y, c);
                    s += std::fabs(p - q);
                    ++n;
                }
            }
        }
        return s / std::max(1, n);
    };

    const double nr = roughness(noisy);
    const double dr = roughness(*denoised);
    CHECK(dr < nr);
}

TEST_CASE("AiDenoiseProcessor: denoise works on a spread of image sizes",
          "[ai][denoise]") {
    struct Case { int w; int h; };
    const std::array<Case, 3> cases = {{{8, 8}, {512, 512}, {1025, 767}}};
    AiDenoiseOptions opts;
    opts.strength = 0.5f;
    AiDenoiseProcessor proc(nullptr);

    uint32_t seed = 1;
    for (const auto& c : cases) {
        INFO("w=" << c.w << " h=" << c.h);
        auto in = makeNoisyBuffer(c.w, c.h, seed++);
        auto out = proc.denoise(in, "", opts);
        REQUIRE(out);
        CHECK(out->width() == c.w);
        CHECK(out->height() == c.h);

        for (int ch = 0; ch < 3; ++ch) {
            const float tl = out->pixel(0, 0, ch);
            const float br = out->pixel(c.w - 1, c.h - 1, ch);
            CHECK(!std::isnan(tl));
            CHECK(!std::isnan(br));
            CHECK(tl >= -0.01f);
            CHECK(br <= 1.01f);
        }
    }
}

TEST_CASE("AiDenoise: Adjustments.aiDenoise hasEffect reflects strength+enabled",
          "[ai][denoise][adjustments]") {
    Adjustments a;
    CHECK_FALSE(a.aiDenoise.hasEffect());
    a.aiDenoise.enabled = true;
    // strength is 100% by default — hasEffect must become true now.
    CHECK(a.aiDenoise.hasEffect());
    a.aiDenoise.strength = 0.0f;
    CHECK_FALSE(a.aiDenoise.hasEffect());
    a.aiDenoise.strength = 0.5f;
    a.aiDenoise.enabled = false;
    CHECK_FALSE(a.aiDenoise.hasEffect());
}

} // namespace lrc::tests
