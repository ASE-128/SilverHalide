#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>

#include "engine/ImageBuffer.h"
#include "engine/Pipeline.h"

using namespace Catch;
using namespace lrc;

namespace {

std::shared_ptr<ImageBuffer> makeFlat(int w, int h, float r, float g, float b) {
    auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
    float* p = reinterpret_cast<float*>(buf->data().data());
    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n; ++i) {
        p[i * 3 + 0] = r;
        p[i * 3 + 1] = g;
        p[i * 3 + 2] = b;
    }
    return buf;
}

std::shared_ptr<ImageBuffer> makeTwoTone(int w, int h, float r1, float g1,
                                         float b1, float r2, float g2, float b2,
                                         double firstFrac) {
    auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
    float* p = reinterpret_cast<float*>(buf->data().data());
    const size_t n = static_cast<size_t>(w) * h;
    const size_t first = static_cast<size_t>(n * firstFrac);
    for (size_t i = 0; i < n; ++i) {
        const bool top = i < first;
        p[i * 3 + 0] = top ? r1 : r2;
        p[i * 3 + 1] = top ? g1 : g2;
        p[i * 3 + 2] = top ? b1 : b2;
    }
    return buf;
}

// A central bright square (cx,cy 半径 cyR 归一化) on a black background.
std::shared_ptr<ImageBuffer> makeCentralObject(int w, int h, float cx, float cy,
                                               float radius) {
    auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
    float* p = reinterpret_cast<float*>(buf->data().data());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float nx = static_cast<float>(x) / (w - 1);
            const float ny = static_cast<float>(y) / (h - 1);
            const bool inside = std::abs(nx - cx) <= radius && std::abs(ny - cy) <= radius;
            const float v = inside ? 0.6f : 0.01f;
            p[static_cast<size_t>(y) * w * 3 + x * 3 + 0] = v;
            p[static_cast<size_t>(y) * w * 3 + x * 3 + 1] = v;
            p[static_cast<size_t>(y) * w * 3 + x * 3 + 2] = v;
        }
    }
    return buf;
}

} // namespace

TEST_CASE("AutoDevelop: mid-grey image needs no correction and no crop",
          "[ai][autodevelop]") {
    auto buf = makeFlat(64, 64, 0.18f, 0.18f, 0.18f);
    const auto s = Pipeline::autoDevelop(*buf);
    REQUIRE(s.exposure == Approx(0.0).margin(0.05));
    REQUIRE(s.temperature == Approx(0.0).margin(1e-6));
    REQUIRE(s.tint == Approx(0.0).margin(1e-6));
    // Uniform frame -> no strong edges -> no crop suggestion.
    REQUIRE_FALSE(s.hasCrop);
}

TEST_CASE("AutoDevelop: dark image lifts exposure (AC-1)", "[ai][autodevelop]") {
    auto buf = makeFlat(64, 64, 0.09f, 0.09f, 0.09f);
    const auto s = Pipeline::autoDevelop(*buf);
    REQUIRE(s.exposure > 0.0);
    REQUIRE(s.exposure == Approx(1.0).margin(0.1));
}

TEST_CASE("AutoDevelop: warm cast gives a negative temperature (AC-3)",
          "[ai][autodevelop]") {
    auto buf = makeFlat(64, 64, 0.6f, 0.5f, 0.4f);
    const auto s = Pipeline::autoDevelop(*buf);
    REQUIRE(s.temperature < 0.0);
}

TEST_CASE("AutoDevelop: finite and in-range fields (AC-1)", "[ai][autodevelop]") {
    const auto black = makeFlat(64, 64, 0.0f, 0.0f, 0.0f);
    const auto white = makeFlat(64, 64, 1.0f, 1.0f, 1.0f);
    for (const auto& buf : {black, white}) {
        const auto s = Pipeline::autoDevelop(*buf);
        REQUIRE(s.exposure >= -3.0);
        REQUIRE(s.exposure <= 3.0);
        REQUIRE(s.contrast >= -50.0);
        REQUIRE(s.contrast <= 50.0);
        REQUIRE(s.temperature >= -100.0);
        REQUIRE(s.temperature <= 100.0);
        REQUIRE(s.tint >= -100.0);
        REQUIRE(s.tint <= 100.0);
        REQUIRE(s.crop.left >= 0.0f);
        REQUIRE(s.crop.top >= 0.0f);
        REQUIRE(s.crop.right <= 1.0f);
        REQUIRE(s.crop.bottom <= 1.0f);
        if (s.hasCrop) {
            REQUIRE(s.crop.right > s.crop.left);
            REQUIRE(s.crop.bottom > s.crop.top);
        }
    }
}

TEST_CASE("AutoDevelop: does not mutate the input buffer (AC-2)",
          "[ai][autodevelop]") {
    auto buf = makeTwoTone(80, 80, 0.8f, 0.5f, 0.3f, 0.1f, 0.1f, 0.1f, 0.5);
    const auto before = buf->data();
    (void)Pipeline::autoDevelop(*buf);
    REQUIRE(buf->data() == before);  // non-destructive contract
}

TEST_CASE("AutoDevelop: deterministic for identical input", "[ai][autodevelop]") {
    auto buf = makeTwoTone(80, 80, 0.8f, 0.5f, 0.3f, 0.1f, 0.1f, 0.1f, 0.5);
    const auto a = Pipeline::autoDevelop(*buf);
    const auto b = Pipeline::autoDevelop(*buf);
    REQUIRE(a.exposure == b.exposure);
    REQUIRE(a.contrast == b.contrast);
    REQUIRE(a.temperature == b.temperature);
    REQUIRE(a.tint == b.tint);
    REQUIRE(a.hasCrop == b.hasCrop);
}

TEST_CASE("AutoDevelop: empty buffer returns defaults", "[ai][autodevelop]") {
    const auto s = Pipeline::autoDevelop(ImageBuffer{});
    REQUIRE(s.exposure == 0.0);
    REQUIRE(s.contrast == 0.0);
    REQUIRE(s.temperature == 0.0);
    REQUIRE(s.tint == 0.0);
    REQUIRE_FALSE(s.hasCrop);
}

TEST_CASE("AutoDevelop: central object yields a centered crop suggestion",
          "[ai][autodevelop]") {
    auto buf = makeCentralObject(256, 256, 0.5f, 0.5f, 0.2f);
    const auto s = Pipeline::autoDevelop(*buf);
    REQUIRE(s.hasCrop);
    // Crop box is strictly inside the frame and roughly centered.
    REQUIRE(s.crop.left > 0.0f);
    REQUIRE(s.crop.right < 1.0f);
    REQUIRE(s.crop.top > 0.0f);
    REQUIRE(s.crop.bottom < 1.0f);
    const float cx = (s.crop.left + s.crop.right) * 0.5f;
    const float cy = (s.crop.top + s.crop.bottom) * 0.5f;
    REQUIRE(cx == Approx(0.5f).margin(0.25f));
    REQUIRE(cy == Approx(0.5f).margin(0.25f));
}

TEST_CASE("AutoDevelop: off-centre object yields an off-centre crop",
          "[ai][autodevelop]") {
    auto buf = makeCentralObject(256, 256, 0.25f, 0.25f, 0.15f);
    const auto s = Pipeline::autoDevelop(*buf);
    REQUIRE(s.hasCrop);
    const float cx = (s.crop.left + s.crop.right) * 0.5f;
    REQUIRE(cx < 0.5f);  // centre of interest is on the left
}
