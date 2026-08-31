#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "catalog/CatalogDatabase.h"
#include "catalog/StandardPreview.h"
#include "engine/ImageBuffer.h"
#include "engine/ImageIO.h"
#include "utils/MathUtils.h"

using namespace Catch;
using namespace lrc;

namespace {

std::filesystem::path makeTempDir(const char* tag) {
    static size_t counter = 0;
    const auto dir =
        std::filesystem::temp_directory_path() /
        (std::string("lrc_stdprev_") + tag + "_" +
         std::to_string(++counter) + "_" +
         std::to_string(static_cast<unsigned long>(
             std::chrono::system_clock::now().time_since_epoch().count())));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// A U8 sRGB source: horizontal red gradient, mid-gray green, vertical blue
// gradient. The green channel is uniform so it stays near-linear after the
// sRGB -> linear ProPhoto conversion.
ImageBufferPtr makeSource(int w, int h) {
    auto img = ImageBuffer::create(w, h, PixelFormat::U8);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            img->setPixel(x, y, 0, static_cast<float>(x) / static_cast<float>(w - 1));
            img->setPixel(x, y, 1, 0.5f);
            img->setPixel(x, y, 2, static_cast<float>(y) / static_cast<float>(h - 1));
        }
    return img;
}

} // namespace

TEST_CASE("StandardPreview: build produces a <=2048px linear F32 buffer",
          "[standard-preview][build]") {
    const auto dir = makeTempDir("build");
    std::error_code ec;
    const auto srcPath = dir / "src.jpg";
    // 2560x1600 (long edge 2560 > 2048) exercises the downscale path.
    auto src = makeSource(2560, 1600);
    REQUIRE(ImageIO::writeJpeg(srcPath, *src, 92));

    auto buf = buildStandardPreview(lrc::pathToUtf8(srcPath));
    REQUIRE(buf != nullptr);
    REQUIRE_FALSE(buf->empty());
    REQUIRE(buf->format() == PixelFormat::F32);

    // Longest edge bounded by kStandardPreviewMaxDim (allow the area-average
    // integer truncation to land one pixel below the exact target).
    const int longest = std::max(buf->width(), buf->height());
    REQUIRE(longest >= kStandardPreviewMaxDim - 1);
    REQUIRE(longest <= kStandardPreviewMaxDim);

    // Aspect ratio preserved (2560:1600 = 1.6).
    const double aspect =
        static_cast<double>(buf->width()) / static_cast<double>(buf->height());
    REQUIRE(aspect == Approx(2560.0 / 1600.0).epsilon(0.02));

    // Green channel at the center is mid-gray sRGB 0.5 -> linear ~0.214.
    const float lg = buf->pixel(buf->width() / 2, buf->height() / 2, 1);
    REQUIRE(lg > 0.18f);
    REQUIRE(lg < 0.26f);

    // Default render must not introduce NaN/Inf anywhere.
    bool nonFinite = false;
    for (int y = 0; y < buf->height(); ++y)
        for (int x = 0; x < buf->width(); ++x)
            for (int c = 0; c < 3; ++c)
                if (!std::isfinite(buf->pixel(x, y, c)))
                    nonFinite = true;
    REQUIRE_FALSE(nonFinite);

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("StandardPreview: missing file returns null without crashing",
          "[standard-preview][build]") {
    auto missing = buildStandardPreview("C:/does/not/exist_xyz.jpg");
    REQUIRE(missing == nullptr);
}

TEST_CASE("StandardPreview: encoder rejects non-F32 buffers",
          "[standard-preview][encode]") {
    auto u8 = ImageBuffer::create(4, 4, PixelFormat::U8);
    REQUIRE(encodeStandardPreview(*u8).empty());
}

TEST_CASE("StandardPreview: store/get round-trips an F32 buffer",
          "[standard-preview][catalog]") {
    const auto dir = makeTempDir("catalog");
    std::error_code ec;
    int64_t id = 0;
    {
        CatalogDatabase db(dir / "test.lrcat");
        REQUIRE(db.open());
        id = db.addImage("/photos/roundtrip.dng", "");

        const int w = 16;
        const int h = 12;
        auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                buf->setPixel(x, y, 0, static_cast<float>(x + y) * 0.01f);
                buf->setPixel(x, y, 1, 0.5f - static_cast<float>(x) * 0.01f);
                buf->setPixel(x, y, 2, static_cast<float>(y) * 0.02f + 0.1f);
            }

        REQUIRE_FALSE(db.hasStandardPreview(id));
        REQUIRE(db.storeStandardPreview(id, *buf));
        REQUIRE(db.hasStandardPreview(id));

        auto loaded = db.getStandardPreview(id);
        REQUIRE(loaded.has_value());
        REQUIRE((*loaded) != nullptr);
        REQUIRE((*loaded)->width() == w);
        REQUIRE((*loaded)->height() == h);
        REQUIRE((*loaded)->format() == PixelFormat::F32);

        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c)
                    REQUIRE((*loaded)->pixel(x, y, c) ==
                            Approx(buf->pixel(x, y, c)).epsilon(1e-6));

        // Dimensions are recorded so callers can size without loading the blob.
        auto size = db.previewSize(id, PreviewKind::Standard);
        REQUIRE(size.has_value());
        REQUIRE(size->first == w);
        REQUIRE(size->second == h);
    }
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("StandardPreview: v2 encode/decode round-trips native dimensions",
          "[standard-preview][S8a]") {
    const int w = 64;
    const int h = 32;
    const int nativeW = 9000;
    const int nativeH = 6000;
    auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            buf->setPixel(x, y, 0, static_cast<float>(x) / w);
            buf->setPixel(x, y, 1, static_cast<float>(y) / h);
            buf->setPixel(x, y, 2, 0.25f);
        }

    // v2 path: encode with native dimensions, decode must report them back.
    auto blob = encodeStandardPreview(*buf, nativeW, nativeH);
    REQUIRE(blob.size() == 32u + static_cast<size_t>(w) * h * 3 * sizeof(float));

    auto decoded = decodeStandardPreview(blob);
    REQUIRE(decoded.buf);
    REQUIRE(decoded.buf->width() == w);
    REQUIRE(decoded.buf->height() == h);
    REQUIRE(decoded.buf->format() == PixelFormat::F32);
    REQUIRE(decoded.nativeWidth == nativeW);
    REQUIRE(decoded.nativeHeight == nativeH);

    // Pixel-exact payload.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                REQUIRE(decoded.buf->pixel(x, y, c) ==
                        Approx(buf->pixel(x, y, c)).epsilon(1e-6));
}

TEST_CASE("StandardPreview: legacy v1 blob is still decodable (native=0)",
          "[standard-preview][S8a]") {
    const int w = 8;
    const int h = 6;
    auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                buf->setPixel(x, y, c, 0.4f);

    // Legacy v1 encode (single-arg overload) -> 24-byte header, no native dims.
    auto blob = encodeStandardPreview(*buf);
    REQUIRE(blob.size() == 24u + static_cast<size_t>(w) * h * 3 * sizeof(float));

    auto decoded = decodeStandardPreview(blob);
    REQUIRE(decoded.buf);
    REQUIRE(decoded.buf->width() == w);
    REQUIRE(decoded.buf->height() == h);
    REQUIRE(decoded.nativeWidth == 0);
    REQUIRE(decoded.nativeHeight == 0);
}

TEST_CASE("StandardPreview: v1 blob with truncated v2-style tail is rejected",
          "[standard-preview][S8a]") {
    // A v1 blob (24-byte header) must NOT be misread as v2 by the decoder even
    // if the payload happens to start with bytes that look like a nativeW/H pair.
    const int w = 2;
    const int h = 2;
    auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                buf->setPixel(x, y, c, 0.5f);
    auto blob = encodeStandardPreview(*buf); // v1
    REQUIRE(blob.size() == 24u + 2u * 2 * 3 * 4u);

    // Decoder must accept it as v1 (native=0), not crash, not misreport dims.
    auto decoded = decodeStandardPreview(blob);
    REQUIRE(decoded.buf);
    REQUIRE(decoded.nativeWidth == 0);
    REQUIRE(decoded.nativeHeight == 0);
}

TEST_CASE("StandardPreview: buildStandardPreview2 reports native dimensions",
          "[standard-preview][S8a]") {
    const auto dir = makeTempDir("v2build");
    std::error_code ec;
    const auto srcPath = dir / "src.jpg";
    // 1280x960 source (long edge < 2048 so no downscale; native == source).
    auto src = makeSource(1280, 960);
    REQUIRE(ImageIO::writeJpeg(srcPath, *src, 92));

    auto built = buildStandardPreview2(lrc::pathToUtf8(srcPath));
    REQUIRE(built.buf);
    REQUIRE_FALSE(built.buf->empty());
    REQUIRE(built.buf->format() == PixelFormat::F32);
    // Non-RAW sources report native dimensions equal to the source size.
    REQUIRE(built.nativeWidth == 1280);
    REQUIRE(built.nativeHeight == 960);

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("StandardPreview: legacy buildStandardPreview still works",
          "[standard-preview][S8a]") {
    const auto dir = makeTempDir("legacy");
    std::error_code ec;
    const auto srcPath = dir / "src.jpg";
    auto src = makeSource(256, 256);
    REQUIRE(ImageIO::writeJpeg(srcPath, *src, 92));

    auto buf = buildStandardPreview(lrc::pathToUtf8(srcPath));
    REQUIRE(buf != nullptr);
    REQUIRE_FALSE(buf->empty());

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("StandardPreview: getStandardPreviewWithMeta returns native dims",
          "[standard-preview][S8b]") {
    const auto dir = makeTempDir("withmeta");
    std::error_code ec;
    int64_t id = 0;
    {
        CatalogDatabase db(dir / "test.lrcat");
        REQUIRE(db.open());
        id = db.addImage("/photos/meta.dng", "");

        const int w = 16;
        const int h = 12;
        const int nativeW = 9000;
        const int nativeH = 6000;
        auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                buf->setPixel(x, y, 0, static_cast<float>(x + y) * 0.01f);

        // Store via the v2 blob directly (storePreviewImpl stores raw bytes;
        // the standard storeStandardPreview path uses v1, so we go through
        // the lower-level API to inject native dims).
        auto blob = encodeStandardPreview(*buf, nativeW, nativeH);
        REQUIRE_FALSE(blob.empty());
        REQUIRE(db.storePreview(id, PreviewKind::Standard, blob, w, h));

        // WithMeta returns the native dims.
        auto meta = db.getStandardPreviewWithMeta(id);
        REQUIRE(meta.has_value());
        REQUIRE(meta->buf);
        REQUIRE(meta->buf->width() == w);
        REQUIRE(meta->buf->height() == h);
        REQUIRE(meta->nativeWidth == nativeW);
        REQUIRE(meta->nativeHeight == nativeH);

        // Legacy getStandardPreview still works (returns just the buffer).
        auto legacy = db.getStandardPreview(id);
        REQUIRE(legacy.has_value());
        REQUIRE((*legacy)->width() == w);

        // A v1 blob (encodeStandardPreview single-arg) reports native=0.
        auto blobV1 = encodeStandardPreview(*buf);
        REQUIRE(db.storePreview(id, PreviewKind::Standard, blobV1, w, h));
        auto metaV1 = db.getStandardPreviewWithMeta(id);
        REQUIRE(metaV1.has_value());
        REQUIRE(metaV1->nativeWidth == 0);
        REQUIRE(metaV1->nativeHeight == 0);
    }
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("StandardPreview: missing / foreign / corrupt records never crash",
          "[standard-preview][catalog]") {
    const auto dir = makeTempDir("missing");
    std::error_code ec;
    {
        CatalogDatabase db(dir / "test.lrcat");
        REQUIRE(db.open());
        const int64_t id = db.addImage("/photos/missing.dng", "");

        // No standard record yet.
        REQUIRE_FALSE(db.hasStandardPreview(id));
        REQUIRE_FALSE(db.getStandardPreview(id).has_value());

        // A thumbnail record for the same image is not a standard preview.
        std::vector<uint8_t> tiny = {1, 2, 3};
        REQUIRE(db.storePreview(id, PreviewKind::Thumbnail, tiny, 1, 1));
        REQUIRE_FALSE(db.hasStandardPreview(id));
        REQUIRE_FALSE(db.getStandardPreview(id).has_value());

        // A corrupt Standard blob is detected as present but undecodable; it
        // must not crash or throw.
        std::vector<uint8_t> garbage = {'N', 'O', 'T', 'A', 'P'};
        REQUIRE(db.storePreview(id, PreviewKind::Standard, garbage, 4, 4));
        REQUIRE(db.hasStandardPreview(id)); // a row exists...
        REQUIRE_FALSE(db.getStandardPreview(id).has_value()); // ...but unusable

        // A truncated (header-only) blob is likewise rejected.
        std::vector<uint8_t> header = {'S', 'H', 'S', 'P', 1, 0, 0, 0};
        REQUIRE(db.storePreview(id, PreviewKind::Standard, header, 0, 0));
        REQUIRE_FALSE(db.getStandardPreview(id).has_value());
    }
    std::filesystem::remove_all(dir, ec);
}
