// P3-1 linear-light histogram tests.
//
// Verifies Pipeline::computeHistogramFromLinear: bin counts must be EXACT
// (full traversal, no sampling) for known synthetic distributions, across the
// F32 / U16 / U8 storage formats.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/ImageBuffer.h"
#include "engine/Pipeline.h"

namespace {

// Inverse of the sRGB OETF (display-encoded [0,1] -> linear [0,1]). Used to
// synthesize a linear value whose display-encoded counterpart lands on the
// centre of a known bin, so bin counts are exact no matter the LUT resolution
// inside the engine (the centre is farthest from both bin boundaries).
float srgbDecode(float enc) {
    enc = (enc < 0.0f) ? 0.0f : (enc > 1.0f ? 1.0f : enc);
    return (enc <= 0.04045f) ? enc / 12.92f
                             : std::pow((enc + 0.055f) / 1.055f, 2.4f);
}

// Linear value whose display-encoded counterpart sits at the centre of `bin`:
// floor((bin + 0.5) / 256 * 256) == bin, i.e. exactly `bin` and farthest from
// both neighbouring boundaries.
float linearAtBinCentre(int bin) {
    return srgbDecode((static_cast<float>(bin) + 0.5f) / 256.0f);
}

// Fills every channel of an existing F32 buffer with `value`.
void fillF32(lrc::ImageBuffer& buf, float value) {
    float* p = reinterpret_cast<float*>(buf.data().data());
    const size_t n =
        static_cast<size_t>(buf.width()) * buf.height() * 3u;
    for (size_t i = 0; i < n; ++i)
        p[i] = value;
}

} // namespace

TEST_CASE("P3-1 histogram: empty buffer yields all-zero bins",
          "[histogram][p3-1]") {
    lrc::ImageBuffer empty(0, 0, lrc::PixelFormat::F32);
    const auto bins = lrc::Pipeline::computeHistogramFromLinear(empty);
    for (const auto& ch : bins)
        for (uint32_t v : ch)
            REQUIRE(v == 0);
}

TEST_CASE("P3-1 histogram: black and white frames hit the extreme bins exactly",
          "[histogram][p3-1]") {
    constexpr int w = 320, h = 240;
    const uint64_t n = static_cast<uint64_t>(w) * h;

    auto black = lrc::ImageBuffer::create(w, h, lrc::PixelFormat::F32);
    fillF32(*black, 0.0f);
    auto bins = lrc::Pipeline::computeHistogramFromLinear(*black);
    for (int c = 0; c < 3; ++c) {
        REQUIRE(bins[c][0] == n);
        for (int b = 1; b < 256; ++b)
            REQUIRE(bins[c][b] == 0);
    }

    auto white = lrc::ImageBuffer::create(w, h, lrc::PixelFormat::F32);
    fillF32(*white, 1.0f);
    bins = lrc::Pipeline::computeHistogramFromLinear(*white);
    for (int c = 0; c < 3; ++c) {
        REQUIRE(bins[c][255] == n);
        for (int b = 0; b < 255; ++b)
            REQUIRE(bins[c][b] == 0);
    }
}

TEST_CASE("P3-1 histogram: constant mid-tone maps to one exact bin",
          "[histogram][p3-1]") {
    // Odd dimensions catch any off-by-one indexing in the traversal loop.
    constexpr int w = 97, h = 53;
    const uint64_t n = static_cast<uint64_t>(w) * h;
    const int targetBin = 100;

    auto buf = lrc::ImageBuffer::create(w, h, lrc::PixelFormat::F32);
    fillF32(*buf, linearAtBinCentre(targetBin));
    const auto bins = lrc::Pipeline::computeHistogramFromLinear(*buf);
    for (int c = 0; c < 3; ++c) {
        REQUIRE(bins[c][targetBin] == n);
        for (int b = 0; b < 256; ++b)
            if (b != targetBin)
                REQUIRE(bins[c][b] == 0);
    }
}

TEST_CASE("P3-1 histogram: channels are counted independently",
          "[histogram][p3-1]") {
    constexpr int w = 64, h = 64;
    const uint64_t n = static_cast<uint64_t>(w) * h;
    const int target[3] = {40, 150, 230};

    auto buf = lrc::ImageBuffer::create(w, h, lrc::PixelFormat::F32);
    float* p = reinterpret_cast<float*>(buf->data().data());
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i, p += 3) {
        p[0] = linearAtBinCentre(target[0]);
        p[1] = linearAtBinCentre(target[1]);
        p[2] = linearAtBinCentre(target[2]);
    }
    const auto out = lrc::Pipeline::computeHistogramFromLinear(*buf);
    for (int c = 0; c < 3; ++c) {
        REQUIRE(out[c][target[c]] == n);
        uint64_t other = 0;
        for (int b = 0; b < 256; ++b)
            if (b != target[c])
                other += out[c][b];
        REQUIRE(other == 0);
    }
}

TEST_CASE("P3-1 histogram: full traversal, no sampling on large frames",
          "[histogram][p3-1]") {
    // 8.4M pixels: the legacy 8-bit preview histogram subsampled every 2nd
    // pixel at this size (step = pixels/4M), which would have produced half
    // the count. The linear path must count EVERY pixel exactly.
    constexpr int w = 4096, h = 2048;
    const uint64_t n = static_cast<uint64_t>(w) * h;
    const int targetBin = 100;

    auto buf = lrc::ImageBuffer::create(w, h, lrc::PixelFormat::F32);
    fillF32(*buf, linearAtBinCentre(targetBin));
    const auto bins = lrc::Pipeline::computeHistogramFromLinear(*buf);
    REQUIRE(bins[0][targetBin] == n);
    REQUIRE(bins[1][targetBin] == n);
    REQUIRE(bins[2][targetBin] == n);
}

TEST_CASE("P3-1 histogram: out-of-range linear values clamp to the ends",
          "[histogram][p3-1]") {
    constexpr int w = 16, h = 16;
    const uint64_t n = static_cast<uint64_t>(w) * h;

    auto buf = lrc::ImageBuffer::create(w, h, lrc::PixelFormat::F32);
    fillF32(*buf, -0.25f);  // below black clamps into bin 0
    auto bins = lrc::Pipeline::computeHistogramFromLinear(*buf);
    REQUIRE(bins[0][0] == n);
    REQUIRE(bins[1][0] == n);

    fillF32(*buf, 1.5f);  // above white clamps into bin 255
    bins = lrc::Pipeline::computeHistogramFromLinear(*buf);
    REQUIRE(bins[0][255] == n);
    REQUIRE(bins[2][255] == n);
}

TEST_CASE("P3-1 histogram: U8 fallback counts stored bytes as bin indices",
          "[histogram][p3-1]") {
    constexpr int w = 80, h = 60;
    const uint64_t n = static_cast<uint64_t>(w) * h;
    auto buf = lrc::ImageBuffer::create(w, h, lrc::PixelFormat::U8);
    std::fill(buf->data().begin(), buf->data().end(),
              static_cast<uint8_t>(128));
    const auto bins = lrc::Pipeline::computeHistogramFromLinear(*buf);
    REQUIRE(bins[0][128] == n);
    REQUIRE(bins[1][128] == n);
    REQUIRE(bins[2][128] == n);
}

TEST_CASE("P3-1 histogram: U16 linear extremes map to the end bins",
          "[histogram][p3-1]") {
    constexpr int w = 40, h = 30;
    const uint64_t n = static_cast<uint64_t>(w) * h;
    auto buf = lrc::ImageBuffer::create(w, h, lrc::PixelFormat::U16);
    auto* p = reinterpret_cast<uint16_t*>(buf->data().data());
    const size_t len = static_cast<size_t>(w) * h * 3u;

    std::fill(p, p + len, uint16_t(0));
    auto bins = lrc::Pipeline::computeHistogramFromLinear(*buf);
    REQUIRE(bins[0][0] == n);

    std::fill(p, p + len, uint16_t(65535));
    bins = lrc::Pipeline::computeHistogramFromLinear(*buf);
    REQUIRE(bins[0][255] == n);
}
