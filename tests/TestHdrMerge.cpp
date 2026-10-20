#include <cstring>
#include <span>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/HdrMerge.h"
#include "engine/ImageBuffer.h"

using namespace Catch;
using namespace lrc;

namespace {

// A flat F32 buffer filled with a single luminance (neutral grey).
ImageBufferPtr flatGrey(int w, int h, float v) {
    auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            buf->setPixel(x, y, 0, v);
            buf->setPixel(x, y, 1, v);
            buf->setPixel(x, y, 2, v);
        }
    return buf;
}

} // namespace

TEST_CASE("HdrMerge: estimates a 1-stop offset between two frames", "[hdr]") {
    const auto a = flatGrey(64, 64, 0.1f);
    const auto b = flatGrey(64, 64, 0.2f); // exactly one stop brighter
    const double ev = HdrMerge::estimateEv(*a, *b);
    REQUIRE(ev == Approx(1.0).margin(0.15));
}

TEST_CASE("HdrMerge: merge normalizes exposures to a common level", "[hdr]") {
    const auto dark = flatGrey(64, 64, 0.1f);
    const auto bright = flatGrey(64, 64, 0.4f); // 2 stops brighter
    std::vector<ImageBufferPtr> frames{dark, bright};

    // Supplied offsets: bright frame is +2 stops relative to dark.
    const auto res = HdrMerge::merge(frames, {0.0, 2.0}, false);
    REQUIRE(res.ok());
    // After normalization both frames sit at 0.1, so the blend is 0.1.
    REQUIRE(res.linear->pixel(0, 0, 0) == Approx(0.1f).margin(0.02f));
    REQUIRE(res.evOffsets.size() == 2);
    REQUIRE(res.evOffsets[0] == Approx(0.0));
    REQUIRE(res.evOffsets[1] == Approx(2.0));
}

TEST_CASE("HdrMerge: deghosting suppresses a moving bright blob", "[hdr]") {
    // Frame 1 has a bright "ghost" blob in the corner; frames 2 and 3 are clean.
    auto f1 = flatGrey(32, 32, 0.1f);
    f1->setPixel(0, 0, 0, 0.9f);
    f1->setPixel(0, 0, 1, 0.9f);
    f1->setPixel(0, 0, 2, 0.9f);
    const auto f2 = flatGrey(32, 32, 0.1f);
    const auto f3 = flatGrey(32, 32, 0.1f);
    std::vector<ImageBufferPtr> frames{f1, f2, f3};

    const auto plain = HdrMerge::merge(frames, {0.0, 0.0, 0.0}, false);
    const auto deghosted = HdrMerge::merge(frames, {0.0, 0.0, 0.0}, true);
    REQUIRE(plain.ok());
    REQUIRE(deghosted.ok());

    // The ghost pixel is visibly lifted in the plain blend...
    REQUIRE(plain.linear->pixel(0, 0, 0) > 0.12f);
    // ...but suppressed back near the clean value when deghosting.
    REQUIRE(deghosted.linear->pixel(0, 0, 0) < plain.linear->pixel(0, 0, 0));
    // Clean pixels stay at 0.1 in both paths.
    REQUIRE(plain.linear->pixel(16, 16, 0) == Approx(0.1f).margin(0.02f));
    REQUIRE(deghosted.linear->pixel(16, 16, 0) == Approx(0.1f).margin(0.02f));
}

TEST_CASE("HdrMerge: rejects single-frame input", "[hdr]") {
    const auto single = flatGrey(8, 8, 0.5f);
    std::vector<ImageBufferPtr> frames{single};
    const auto res = HdrMerge::merge(frames, {}, false);
    REQUIRE_FALSE(res.ok());
    REQUIRE_FALSE(res.error.empty());
}

TEST_CASE("HdrMerge: rejects mismatched dimensions", "[hdr]") {
    std::vector<ImageBufferPtr> frames{flatGrey(16, 16, 0.1f),
                                       flatGrey(32, 32, 0.2f)};
    const auto res = HdrMerge::merge(frames, {0.0, 1.0}, false);
    REQUIRE_FALSE(res.ok());
}

TEST_CASE("HdrMerge: F32 span path matches the U16 pixel() fallback exactly",
          "[hdr][m5]") {
    // The luminance downscale has two implementations: the typed F32 span path
    // and the normalized pixel() fallback used for U16/U8 sources. 320x213 is
    // deliberately awkward: the 64px downscale rounds the last target row's
    // sample window past the bottom edge, so the two paths only agree if the
    // clipped samples keep contributing exactly 0.0 while still being counted.
    const int w = 320, h = 213;
    const auto makeU16 = [&](float base, float span) {
        auto buf = ImageBuffer::create(w, h, PixelFormat::U16);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float v =
                    base + span * static_cast<float>(x + y) /
                               static_cast<float>(w + h);
                buf->setPixel(x, y, 0, v);
                buf->setPixel(x, y, 1, v * 0.8f);
                buf->setPixel(x, y, 2, v * 1.1f);
            }
        return buf;
    };
    const auto u1 = makeU16(0.05f, 0.40f);
    const auto u2 = makeU16(0.20f, 0.50f);
    const auto f1 = u1->toF32(); // same floats the U16 pixel() path returns
    const auto f2 = u2->toF32();
    // Non-empty views prove the F32 side really goes through the typed span
    // path rather than silently falling back to pixel().
    REQUIRE(f1->asFloatSpan().size() ==
            static_cast<size_t>(w) * static_cast<size_t>(h) * 3u);
    REQUIRE(u1->asFloatSpan().empty());

    // The two downscale implementations must agree bit-for-bit.
    REQUIRE(HdrMerge::estimateEv(*u1, *u2) == HdrMerge::estimateEv(*f1, *f2));

    const auto fromU16 = HdrMerge::merge({u1, u2}, {}, false, 64);
    const auto fromF32 = HdrMerge::merge({f1, f2}, {}, false, 64);
    REQUIRE(fromU16.ok());
    REQUIRE(fromF32.ok());
    REQUIRE(fromU16.evOffsets == fromF32.evOffsets);
    REQUIRE(fromU16.linear->data() == fromF32.linear->data());
}

TEST_CASE("HdrMerge: F32 merge blends to the expected per-pixel value", "[hdr]") {
    // Two identical neutral-grey frames at the same exposure: the weighted mean
    // of two equal values is that value, so the merged pixels must be it.
    const auto a = flatGrey(64, 64, 0.25f);
    const auto b = flatGrey(64, 64, 0.25f);
    const auto res = HdrMerge::merge({a, b}, {0.0, 0.0}, false);
    REQUIRE(res.ok());
    for (const int y : {0, 31, 63})
        for (const int x : {0, 17, 63})
            for (int c = 0; c < 3; ++c)
                REQUIRE(res.linear->pixel(x, y, c) == Approx(0.25f).margin(1e-6f));

    // The mutable typed view must expose the same interleaved bytes that the
    // normalized accessor reads back.
    const std::span<const float> v = res.linear->asFloatSpan();
    REQUIRE(v.size() == 64u * 64u * 3u);
    REQUIRE(v[0] == res.linear->pixel(0, 0, 0));
    REQUIRE(v[(static_cast<size_t>(63) * 64 + 63) * 3 + 2] ==
            res.linear->pixel(63, 63, 2));
}

TEST_CASE("HdrMerge: banded merge is bit-identical to whole-frame merge",
          "[hdr][m5]") {
    // 160 rows (> the 128-row default band) so a small bandRows splits the
    // image into several bands; compare byte-for-byte against a single-band
    // merge. Both the exposure-estimation branch (empty evOffsets) and the
    // deghost paths must stay bit-identical.
    const int w = 160, h = 160;
    const auto makeFrame = [&](float base, int ghost) {
        auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float g =
                    0.05f + 0.5f * static_cast<float>(x + y) / (w + h);
                const float v = base + g;
                buf->setPixel(x, y, 0, v);
                buf->setPixel(x, y, 1, v * 0.8f);
                buf->setPixel(x, y, 2, v * 1.1f);
                if (x < ghost && y < ghost)
                    buf->setPixel(x, y, 0, 0.9f);  // ghost blob
            }
        return buf;
    };
    const std::vector<ImageBufferPtr> frames{
        makeFrame(0.1f, 8), makeFrame(0.4f, 0), makeFrame(0.05f, 0)};
    for (const bool dg : {false, true}) {
        const auto whole = HdrMerge::merge(frames, {}, dg, 100000);
        const auto banded = HdrMerge::merge(frames, {}, dg, 32);
        REQUIRE(whole.ok());
        REQUIRE(banded.ok());
        REQUIRE(whole.linear->width() == banded.linear->width());
        REQUIRE(whole.linear->height() == banded.linear->height());
        const auto& a = whole.linear->data();
        const auto& b = banded.linear->data();
        REQUIRE(a.size() == b.size());
        REQUIRE(std::memcmp(a.data(), b.data(), a.size()) == 0);
    }
}
