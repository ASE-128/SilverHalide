#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <span>

#include "engine/ColorManager.h"
#include "engine/ImageBuffer.h"

using namespace Catch;
using lrc::ColorManager;
using lrc::ImageBuffer;
using lrc::PixelFormat;

TEST_CASE("ImageBuffer: U16 pixel round-trip", "[image]") {
    auto buf = ImageBuffer::create(4, 3, PixelFormat::U16);
    REQUIRE(buf->width() == 4);
    REQUIRE(buf->height() == 3);
    REQUIRE(buf->channels() == 3);
    REQUIRE(buf->sizeBytes() == 4u * 3 * 3 * 2);
    buf->setPixel(1, 1, 0, 0.5f);
    buf->setPixel(1, 1, 1, 1.0f);
    buf->setPixel(1, 1, 2, 0.0f);
    // 0.5 * 65535 = 32767.5 sits between two U16 codes.
    REQUIRE(buf->pixel(1, 1, 0) == Approx(0.5f).margin(1e-4f));
    REQUIRE(buf->pixel(1, 1, 1) == Approx(1.0f));
    REQUIRE(buf->pixel(1, 1, 2) == Approx(0.0f));
}

TEST_CASE("ImageBuffer: F32 and U8 round-trip", "[image]") {
    auto f = ImageBuffer::create(2, 2, PixelFormat::F32);
    f->setPixel(0, 0, 0, 0.123f);
    REQUIRE(f->pixel(0, 0, 0) == Approx(0.123f));

    auto u8 = ImageBuffer::create(2, 2, PixelFormat::U8);
    u8->setPixel(1, 1, 2, 0.5f);
    // 0.5 * 255 = 127.5, which a U8 buffer cannot represent exactly.
    REQUIRE(u8->pixel(1, 1, 2) == Approx(0.5f).margin(0.002f));
}

TEST_CASE("ImageBuffer: crop extracts the requested region", "[image]") {
    auto src = ImageBuffer::create(10, 10, PixelFormat::U16);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x)
            src->setPixel(x, y, 0, static_cast<float>(x) / 9.0f);

    auto cropped = src->crop(2, 2, 3, 3);
    REQUIRE(cropped->width() == 3);
    REQUIRE(cropped->height() == 3);
    // U16 round-trip has ~1 LSB quantization error (~1.5e-5).
    REQUIRE(cropped->pixel(0, 0, 0) == Approx(2.0f / 9.0f).margin(2e-4f));
    REQUIRE(cropped->pixel(2, 2, 0) == Approx(4.0f / 9.0f).margin(2e-4f));
}

TEST_CASE("ImageBuffer: resize downscales dimensions", "[image]") {
    auto src = ImageBuffer::create(100, 50, PixelFormat::U16);
    for (int y = 0; y < 50; ++y)
        for (int x = 0; x < 100; ++x)
            src->setPixel(x, y, 1, 1.0f); // uniform green

    auto small = src->resize(10, 5);
    REQUIRE(small->width() == 10);
    REQUIRE(small->height() == 5);
    REQUIRE(small->format() == PixelFormat::U16);
    // Uniform image stays uniform after area averaging.
    REQUIRE(small->pixel(5, 2, 1) == Approx(1.0f));
}

TEST_CASE("ImageBuffer: resize averages mixed pixels", "[image]") {
    // Left half black, right half white -> each downscaled column averages.
    auto src = ImageBuffer::create(100, 1, PixelFormat::F32);
    for (int x = 0; x < 100; ++x)
        src->setPixel(x, 0, 0, x < 50 ? 0.0f : 1.0f);

    auto small = src->resize(10, 1);
    REQUIRE(small->pixel(2, 0, 0) == Approx(0.0f));
    REQUIRE(small->pixel(7, 0, 0) == Approx(1.0f));
}

TEST_CASE("ImageBuffer: toF32 preserves values", "[image]") {
    auto src = ImageBuffer::create(4, 4, PixelFormat::U16);
    src->setPixel(0, 0, 0, 0.75f);
    auto f = src->toF32();
    REQUIRE(f->format() == PixelFormat::F32);
    REQUIRE(f->pixel(0, 0, 0) == Approx(0.75f));
}

TEST_CASE("ImageBuffer: toF32 on a large buffer preserves values", "[image]") {
    // Height well above the default grain, so the parallel per-row path runs.
    const int w = 257, h = 400;
    auto src = ImageBuffer::create(w, h, PixelFormat::U16);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            src->setPixel(x, y, 0, static_cast<float>(x % 97) / 96.0f);
            src->setPixel(x, y, 1, static_cast<float>(y % 64) / 63.0f);
            src->setPixel(x, y, 2, 0.5f);
        }
    auto f = src->toF32();
    REQUIRE(f->format() == PixelFormat::F32);
    REQUIRE(f->width() == w);
    REQUIRE(f->height() == h);
    for (int y = 0; y < h; y += 7)
        for (int x = 0; x < w; x += 5) {
            // U16 storage quantizes to ~1 LSB (~1.5e-5), so use a small margin.
            REQUIRE(f->pixel(x, y, 0) ==
                    Approx(static_cast<float>(x % 97) / 96.0f).margin(2e-4f));
            REQUIRE(f->pixel(x, y, 1) ==
                    Approx(static_cast<float>(y % 64) / 63.0f).margin(2e-4f));
            REQUIRE(f->pixel(x, y, 2) == Approx(0.5f).margin(2e-4f));
        }
}

// --- M20: boundary behaviours ----------------------------------------------

TEST_CASE("ImageBuffer: out-of-range crop returns an empty buffer", "[image][m20]") {
    auto src = ImageBuffer::create(10, 10, PixelFormat::U16);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x)
            src->setPixel(x, y, 0, static_cast<float>(x) / 9.0f);

    // Negative origin.
    auto a = src->crop(-1, 0, 3, 3);
    REQUIRE(a->empty());
    REQUIRE(a->width() == 0);
    REQUIRE(a->height() == 0);
    // Extends past the right / bottom edge.
    auto b = src->crop(0, 0, 11, 3);
    REQUIRE(b->empty());
    auto c = src->crop(8, 8, 3, 3);
    REQUIRE(c->empty());
    // Non-positive size.
    auto d = src->crop(0, 0, 0, 3);
    REQUIRE(d->empty());
    auto e = src->crop(0, 0, 3, 0);
    REQUIRE(e->empty());
    // The in-range crop is unaffected.
    auto ok = src->crop(1, 1, 3, 3);
    REQUIRE_FALSE(ok->empty());
    REQUIRE(ok->width() == 3);
    REQUIRE(ok->height() == 3);
}

TEST_CASE("ImageBuffer: resize to zero or negative dimensions returns empty",
          "[image][m20]") {
    auto src = ImageBuffer::create(10, 10, PixelFormat::U16);
    auto a = src->resize(0, 0);
    REQUIRE(a->empty());
    auto b = src->resize(-4, 8);
    REQUIRE(b->empty());
    auto c = src->resize(8, 0);
    REQUIRE(c->empty());
    // Resizing an empty buffer also yields empty.
    auto empty = ImageBuffer::create(0, 0, PixelFormat::F32);
    REQUIRE(empty->resize(4, 4)->empty());
}

TEST_CASE("ImageBuffer: to8bitSRGB converts linear ProPhoto to display sRGB",
          "[image][m20]") {
    ColorManager cm;
    auto linear = ImageBuffer::create(4, 4, PixelFormat::F32);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            // Neutral mid-gray: 0.18 linear encodes to ~117 sRGB.
            linear->setPixel(x, y, 0, 0.18f);
            linear->setPixel(x, y, 1, 0.18f);
            linear->setPixel(x, y, 2, 0.18f);
        }

    auto out = linear->to8bitSRGB(cm);
    REQUIRE(out->format() == PixelFormat::U8);
    REQUIRE(out->width() == 4);
    REQUIRE(out->height() == 4);
    // 0.18 linear encodes to ~117 sRGB (matches the ColorManager test).
    REQUIRE(out->pixel(0, 0, 0) == Approx(117.0f / 255.0f).margin(0.02f));
    REQUIRE(std::abs(out->pixel(0, 0, 0) - out->pixel(0, 0, 1)) < 0.02f);
    REQUIRE(std::abs(out->pixel(0, 0, 1) - out->pixel(0, 0, 2)) < 0.02f);
}

// --- typed F32 views --------------------------------------------------------

TEST_CASE("ImageBuffer: F32 span view mirrors the pixel() layout", "[image]") {
    auto buf = ImageBuffer::create(5, 3, PixelFormat::F32);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 5; ++x)
            for (int c = 0; c < 3; ++c)
                buf->setPixel(x, y, c,
                              static_cast<float>(y * 15 + x * 3 + c) / 100.0f);

    const std::span<const float> s = buf->asFloatSpan();
    // Interleaved RGB, row-major: one float per channel of every pixel.
    REQUIRE(s.size() == static_cast<size_t>(buf->width()) *
                            static_cast<size_t>(buf->height()) *
                            static_cast<size_t>(buf->channels()));
    REQUIRE(s.size() == 5u * 3u * 3u);
    // The first element is the red channel of pixel (0,0).
    REQUIRE(s[0] == buf->pixel(0, 0, 0));
    // Every index maps to exactly the value pixel() reports.
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 5; ++x)
            for (int c = 0; c < 3; ++c)
                REQUIRE(s[(static_cast<size_t>(y) * 5 + x) * 3 + c] ==
                        buf->pixel(x, y, c));
}

TEST_CASE("ImageBuffer: mutable F32 span writes are visible through pixel()",
          "[image]") {
    auto buf = ImageBuffer::create(4, 2, PixelFormat::F32);
    std::span<float> s = buf->asMutableFloatSpan();
    REQUIRE(s.size() == 4u * 2u * 3u);
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = static_cast<float>(i) / 31.0f;

    REQUIRE(buf->pixel(0, 0, 0) == 0.0f);
    REQUIRE(buf->pixel(1, 0, 2) == 5.0f / 31.0f);
    REQUIRE(buf->pixel(3, 1, 2) == 23.0f / 31.0f);
    // Writes through the view and reads through the view stay consistent.
    s[(static_cast<size_t>(1) * 4 + 2) * 3 + 1] = 0.75f;
    REQUIRE(buf->pixel(2, 1, 1) == 0.75f);
}

TEST_CASE("ImageBuffer: span views are empty for non-F32 and empty buffers",
          "[image]") {
    for (PixelFormat fmt : {PixelFormat::U16, PixelFormat::U8}) {
        auto b = ImageBuffer::create(4, 4, fmt);
        REQUIRE(b->asFloatSpan().empty());
        REQUIRE(b->asFloatSpan().size() == 0u);
        REQUIRE(b->asMutableFloatSpan().empty());
    }

    auto emptyF32 = ImageBuffer::create(0, 0, PixelFormat::F32);
    REQUIRE(emptyF32->format() == PixelFormat::F32);
    REQUIRE(emptyF32->asFloatSpan().empty());
    REQUIRE(emptyF32->asMutableFloatSpan().empty());

    // A default-constructed buffer is empty and F32 by default.
    ImageBuffer def;
    REQUIRE(def.format() == PixelFormat::F32);
    REQUIRE(def.asFloatSpan().empty());
    REQUIRE(def.asMutableFloatSpan().empty());
}

TEST_CASE("ImageBuffer: to8bitSRGB on an empty buffer is empty", "[image][m20]") {
    ColorManager cm;
    auto empty = ImageBuffer::create(0, 0, PixelFormat::F32);
    auto out = empty->to8bitSRGB(cm);
    REQUIRE(out->empty());
    REQUIRE(out->format() == PixelFormat::U8);
}
