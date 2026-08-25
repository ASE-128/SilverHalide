#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/ImageBuffer.h"
#include "engine/PanoramaStitcher.h"

using namespace Catch;
using namespace lrc;

namespace {

// Fills a smooth, non-periodic 2D pattern whose values depend on the absolute
// position in a base canvas of (baseW, baseH). Cropping two windows offset by
// `shift` produces images that are exact horizontal shifts of each other, which
// gives the spatial normalized cross-correlation an unambiguous peak.
void fillPattern(ImageBuffer& buf, int baseW, int baseH) {
    constexpr float k2Pi = 6.28318530718f;
    const int w = buf.width();
    const int h = buf.height();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float nx = static_cast<float>(x) / baseW;
            const float ny = static_cast<float>(y) / baseH;
            const float r = 0.35f + 0.18f * std::sin(k2Pi * 1.7f * nx)
                          + 0.10f * std::sin(k2Pi * 2.9f * ny);
            const float g = 0.35f + 0.15f * std::sin(k2Pi * 2.2f * ny)
                          + 0.10f * std::sin(k2Pi * 4.1f * nx);
            const float b = 0.40f
                          + 0.15f * std::sin(k2Pi * 1.6f * (nx + 0.5f * ny));
            buf.setPixel(x, y, 0, r);
            buf.setPixel(x, y, 1, g);
            buf.setPixel(x, y, 2, b);
        }
    }
}

// Creates `count` images of size (w, h) that are exact horizontal shifts of a
// common base pattern: image i+1 is image i shifted right by `shift` px.
std::vector<ImageBufferPtr> makeSequence(int count, int w, int h, int shift) {
    const int totalW = w + (count - 1) * shift;
    auto base = ImageBuffer::create(totalW, h, PixelFormat::F32);
    fillPattern(*base, totalW, h);
    std::vector<ImageBufferPtr> imgs;
    imgs.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        imgs.push_back(base->crop(i * shift, 0, w, h));
    return imgs;
}

// Creates two images of size (w, h) that are shifted versions of a common
// base: image 1(x, y) == image 0(x + shiftX, y + shiftY). With shiftY > 0 image
// 0 is cropped lower in the base, so image 1 is aligned at (shiftX, -shiftY)
// and the panorama canvas has a negative Y origin -- exercising the negative
// canvas-origin path of the compositor.
std::vector<ImageBufferPtr> makeShiftedPair(int w, int h, int shiftX,
                                            int shiftY) {
    const int baseW = w + shiftX;
    const int baseH = h + shiftY;
    auto base = ImageBuffer::create(baseW, baseH, PixelFormat::F32);
    fillPattern(*base, baseW, baseH);
    std::vector<ImageBufferPtr> imgs;
    imgs.push_back(base->crop(0, shiftY, w, h));   // image 0 (lower)
    imgs.push_back(base->crop(shiftX, 0, w, h));   // image 1 (upper, shifted right)
    return imgs;
}

// True if any pixel in the given column/row range has a channel above `eps`
// (used to detect all-black gap columns/rows).
bool columnHasContent(const ImageBuffer& buf, int x, int y0, int y1,
                      float eps) {
    for (int y = y0; y < y1; ++y)
        if (buf.pixel(x, y, 0) > eps || buf.pixel(x, y, 1) > eps ||
            buf.pixel(x, y, 2) > eps)
            return true;
    return false;
}

bool rowHasContent(const ImageBuffer& buf, int y, int x0, int x1, float eps) {
    for (int x = x0; x < x1; ++x)
        if (buf.pixel(x, y, 0) > eps || buf.pixel(x, y, 1) > eps ||
            buf.pixel(x, y, 2) > eps)
            return true;
    return false;
}

} // namespace

TEST_CASE("PanoramaStitcher: empty input returns nullptr", "[pano]") {
    REQUIRE(stitchPanorama({}) == nullptr);
}

TEST_CASE("PanoramaStitcher: single F32 input returns a same-size F32 copy",
          "[pano]") {
    auto imgs = makeSequence(1, 128, 64, 0);
    const auto out = stitchPanorama(imgs);
    REQUIRE(out);
    REQUIRE(out->width() == 128);
    REQUIRE(out->height() == 64);
    REQUIRE(out->format() == PixelFormat::F32);
    REQUIRE(out->pixel(10, 10, 0) ==
            Approx(imgs[0]->pixel(10, 10, 0)).margin(1e-6f));
    REQUIRE(out->pixel(64, 32, 2) ==
            Approx(imgs[0]->pixel(64, 32, 2)).margin(1e-6f));
}

TEST_CASE("PanoramaStitcher: single U16 input is converted to F32", "[pano]") {
    auto buf = ImageBuffer::create(16, 16, PixelFormat::U16);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            buf->setPixel(x, y, 0, static_cast<float>(x + y) / 30.0f);
    std::vector<ImageBufferPtr> imgs{buf};
    const auto out = stitchPanorama(imgs);
    REQUIRE(out);
    REQUIRE(out->format() == PixelFormat::F32);
    REQUIRE(out->width() == 16);
    REQUIRE(out->height() == 16);
    REQUIRE(out->pixel(3, 4, 0) == Approx(buf->pixel(3, 4, 0)).margin(1e-3f));
}

TEST_CASE("PanoramaStitcher: two-image translation stitch has exact geometry",
          "[pano]") {
    const int shift = 30;
    auto imgs = makeSequence(2, 128, 64, shift);
    PanoramaOptions opts;
    opts.cylindrical = false;
    const auto out = stitchPanorama(imgs, opts);
    REQUIRE(out);
    // Width = w0 + shift (the second image overlaps by w - shift).
    REQUIRE(out->width() == 128 + shift);
    REQUIRE(out->height() == 64);
    // The union of two shifted rectangles is fully covered: no all-black
    // columns and no all-black rows anywhere.
    for (int x = 0; x < out->width(); ++x)
        REQUIRE(columnHasContent(*out, x, 0, out->height(), 0.01f));
    for (int y = 0; y < out->height(); ++y)
        REQUIRE(rowHasContent(*out, y, 0, out->width(), 0.01f));
}

TEST_CASE("PanoramaStitcher: two-image stitch with a vertical offset is safe "
          "(negative canvas origin)", "[pano]") {
    auto imgs = makeShiftedPair(128, 64, 30, 5);
    PanoramaOptions opts;
    opts.cylindrical = false;
    const auto out = stitchPanorama(imgs, opts);
    REQUIRE(out);
    // Image 1 is aligned at (30, -5): the canvas spans a negative Y origin and
    // must not write out of bounds (the compositor's output-row index has to
    // stay inside [0, canvasH)).
    REQUIRE(out->width() == 128 + 30);
    REQUIRE(out->height() == 64 + 5);
    // The union of the two shifted rectangles is fully covered.
    for (int x = 0; x < out->width(); ++x)
        REQUIRE(columnHasContent(*out, x, 0, out->height(), 0.01f));
    for (int y = 0; y < out->height(); ++y)
        REQUIRE(rowHasContent(*out, y, 0, out->width(), 0.01f));
}

TEST_CASE("PanoramaStitcher: cylindrical stitch with a vertical offset is safe "
          "(negative canvas origin)", "[pano]") {
    auto imgs = makeShiftedPair(128, 64, 30, 5);
    PanoramaOptions opts;
    opts.focalLengthMm = 80.0f;
    opts.cylindrical = true;
    const auto out = stitchPanorama(imgs, opts);
    REQUIRE(out);
    REQUIRE(out->width() >= 140);
    REQUIRE(out->width() <= 165);
    REQUIRE(out->height() >= 65);
    REQUIRE(out->height() <= 72);
    const int y0 = out->height() / 4;
    const int y1 = 3 * out->height() / 4;
    for (int x = 0; x < out->width(); ++x)
        REQUIRE(columnHasContent(*out, x, y0, y1, 0.01f));
}

TEST_CASE("PanoramaStitcher: two-image cylindrical stitch is a valid panorama",
          "[pano]") {
    auto imgs = makeSequence(2, 128, 64, 30);
    PanoramaOptions opts;
    opts.focalLengthMm = 80.0f;   // moderate focal length, small barrel distortion
    opts.cylindrical = true;
    const auto out = stitchPanorama(imgs, opts);
    REQUIRE(out);
    REQUIRE(out->width() >= 128);
    REQUIRE(out->width() <= 165);
    REQUIRE(out->height() >= 60);
    REQUIRE(out->height() <= 68);
    // No all-black columns in the central rows (the overlap region).
    const int y0 = out->height() / 4;
    const int y1 = 3 * out->height() / 4;
    for (int x = 0; x < out->width(); ++x)
        REQUIRE(columnHasContent(*out, x, y0, y1, 0.01f));
    // No all-black rows in the central columns.
    const int x0 = out->width() / 4;
    const int x1 = 3 * out->width() / 4;
    for (int y = 0; y < out->height(); ++y)
        REQUIRE(rowHasContent(*out, y, x0, x1, 0.01f));
}

TEST_CASE("PanoramaStitcher: seam is continuous in the overlap region",
          "[pano]") {
    auto imgs = makeSequence(2, 128, 64, 30);
    PanoramaOptions opts;
    opts.focalLengthMm = 80.0f;
    opts.cylindrical = true;
    const auto out = stitchPanorama(imgs, opts);
    REQUIRE(out);
    const int W = out->width();
    const int H = out->height();
    const int y0 = H / 4;
    const int y1 = 3 * H / 4;
    const int x0 = W / 4;
    const int x1 = 3 * W / 4;
    // The central band is inside the overlap region; adjacent pixels must not
    // jump by more than the smooth pattern's own gradient plus a small seam
    // allowance.
    float maxDiff = 0.0f;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1 - 1; ++x)
            for (int c = 0; c < 3; ++c)
                maxDiff = std::max(
                    maxDiff,
                    std::fabs(out->pixel(x + 1, y, c) - out->pixel(x, y, c)));
    REQUIRE(maxDiff < 0.08f);
}

TEST_CASE("PanoramaStitcher: three-image translation sequence has expected width",
          "[pano]") {
    const int shift = 30;
    auto imgs = makeSequence(3, 128, 64, shift);
    PanoramaOptions opts;
    opts.cylindrical = false;
    const auto out = stitchPanorama(imgs, opts);
    REQUIRE(out);
    // Width = w0 + 2 * shift; the middle image is overlapped on both sides.
    REQUIRE(out->width() == 128 + 2 * shift);
    REQUIRE(out->height() == 64);
    for (int x = 0; x < out->width(); ++x)
        REQUIRE(columnHasContent(*out, x, 0, out->height(), 0.01f));
}

TEST_CASE("PanoramaStitcher: three-image cylindrical sequence is a valid panorama",
          "[pano]") {
    auto imgs = makeSequence(3, 128, 64, 30);
    PanoramaOptions opts;
    opts.focalLengthMm = 80.0f;
    opts.cylindrical = true;
    const auto out = stitchPanorama(imgs, opts);
    REQUIRE(out);
    REQUIRE(out->width() >= 150);
    REQUIRE(out->width() <= 200);
    REQUIRE(out->height() >= 60);
    REQUIRE(out->height() <= 68);
    const int y0 = out->height() / 4;
    const int y1 = 3 * out->height() / 4;
    for (int x = 0; x < out->width(); ++x)
        REQUIRE(columnHasContent(*out, x, y0, y1, 0.01f));
}

TEST_CASE("PanoramaStitcher: default options (35 mm cylindrical) still stitch",
          "[pano]") {
    auto imgs = makeSequence(2, 128, 64, 30);
    const auto out = stitchPanorama(imgs);   // all defaults
    REQUIRE(out);
    REQUIRE(out->width() >= 128);
    REQUIRE(out->width() <= 165);
    REQUIRE(out->height() >= 60);
    const int y0 = out->height() / 4;
    const int y1 = 3 * out->height() / 4;
    for (int x = 0; x < out->width(); ++x)
        REQUIRE(columnHasContent(*out, x, y0, y1, 0.01f));
}

TEST_CASE("PanoramaStitcher: banded blending is bit-identical across band sizes",
          "[pano]") {
    auto imgs = makeSequence(2, 128, 64, 30);
    PanoramaOptions o1;
    o1.cylindrical = false;
    o1.bandRows = 2;
    PanoramaOptions o2;
    o2.cylindrical = false;
    o2.bandRows = 1000;
    const auto a = stitchPanorama(imgs, o1);
    const auto b = stitchPanorama(imgs, o2);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(a->width() == b->width());
    REQUIRE(a->height() == b->height());
    REQUIRE(a->data().size() == b->data().size());
    REQUIRE(std::memcmp(a->data().data(), b->data().data(),
                        a->data().size()) == 0);
}

TEST_CASE("PanoramaStitcher: throws on featureless non-overlapping inputs",
          "[pano]") {
    auto red = ImageBuffer::create(128, 64, PixelFormat::F32);
    auto blue = ImageBuffer::create(128, 64, PixelFormat::F32);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 128; ++x) {
            red->setPixel(x, y, 0, 0.8f);
            red->setPixel(x, y, 1, 0.1f);
            red->setPixel(x, y, 2, 0.1f);
            blue->setPixel(x, y, 0, 0.1f);
            blue->setPixel(x, y, 1, 0.1f);
            blue->setPixel(x, y, 2, 0.8f);
        }
    }
    std::vector<ImageBufferPtr> imgs{red, blue};
    REQUIRE_THROWS_AS(stitchPanorama(imgs), std::runtime_error);
}
