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
#include "engine/ColorManager.h"
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

    // v3 encode with native dimensions; decode reports them back.
    auto blob = encodeStandardPreview(*buf, nativeW, nativeH);
    REQUIRE(blob.size() == 40u + static_cast<size_t>(w) * h * 3 * sizeof(float));

    auto decoded = decodeStandardPreview(blob);
    REQUIRE(decoded.buf);
    REQUIRE(decoded.buf->width() == w);
    REQUIRE(decoded.buf->height() == h);
    REQUIRE(decoded.buf->format() == PixelFormat::F32);
    REQUIRE(decoded.nativeWidth == nativeW);
    REQUIRE(decoded.nativeHeight == nativeH);
    // The v3 header records the source-colour identity and the current
    // conversion policy version (R-10); single-arg encode used sourceColor 0.
    REQUIRE(decoded.sourceColor == 0);
    REQUIRE(decoded.colorVersion == kStandardPreviewColorVersion);

    // Pixel-exact payload.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                REQUIRE(decoded.buf->pixel(x, y, c) ==
                        Approx(buf->pixel(x, y, c)).epsilon(1e-6));
}

TEST_CASE("StandardPreview: single-arg encode records native=0 / colour=none",
          "[standard-preview][S8a]") {
    const int w = 8;
    const int h = 6;
    auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                buf->setPixel(x, y, c, 0.4f);

    // Single-arg overload -> current v3 header with no native dims / colour.
    auto blob = encodeStandardPreview(*buf);
    REQUIRE(blob.size() == 40u + static_cast<size_t>(w) * h * 3 * sizeof(float));

    auto decoded = decodeStandardPreview(blob);
    REQUIRE(decoded.buf);
    REQUIRE(decoded.buf->width() == w);
    REQUIRE(decoded.buf->height() == h);
    REQUIRE(decoded.nativeWidth == 0);
    REQUIRE(decoded.nativeHeight == 0);
}

TEST_CASE("StandardPreview: obsolete v1/v2 blobs are rejected as stale (R-10)",
          "[standard-preview][S8a]") {
    // Any blob whose header version is not the current v3 was produced before
    // ICC-aware conversion (R-08/R-09) and must be regenerated, never reused.
    const int w = 2;
    const int h = 2;
    auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                buf->setPixel(x, y, c, 0.5f);

    // A v2 header (32 bytes: magic, version=2, w, h, channels, format, nativeW,
    // nativeH) with a valid payload must be treated as stale.
    std::vector<uint8_t> v2 = {'S', 'H', 'S', 'P'};
    auto pushU32 = [&v2](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            v2.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFFu));
    };
    pushU32(2);  // version
    pushU32(w);
    pushU32(h);
    pushU32(3);                        // channels
    pushU32(2);                        // format F32
    pushU32(4);                        // nativeW
    pushU32(4);                        // nativeH
    const auto* src = buf->data().data();
    v2.insert(v2.end(), src, src + static_cast<size_t>(w) * h * 3 * sizeof(float));

    auto stale = decodeStandardPreview(v2);
    REQUIRE_FALSE(stale.buf);  // stale -> must regenerate (R-10)

    // A current v3 blob from the same pixels still decodes.
    auto current = decodeStandardPreview(encodeStandardPreview(*buf));
    REQUIRE(current.buf);
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

TEST_CASE("StandardPreview: source-colour hash is recorded in the blob (R-10)",
          "[standard-preview][R10]") {
    const auto dir = makeTempDir("r10color");
    std::error_code ec;
    const auto srcPath = dir / "src.jpg";
    auto src = makeSource(640, 400);

    // Untagged JPEG -> fallback sRGB -> sourceColor == 0.
    REQUIRE(ImageIO::writeJpeg(srcPath, *src, 90));
    auto builtPlain = buildStandardPreview2(lrc::pathToUtf8(srcPath));
    REQUIRE(builtPlain.buf);
    REQUIRE(builtPlain.sourceColor == 0);

    // Adobe RGB-tagged JPEG -> non-zero source-colour hash that round-trips
    // through encode/decode. This is the record that lets a future policy
    // change or source-ICC swap invalidate the cached preview (R-10).
    const auto adobe = ColorManager::exportProfileBytes(
        ColorManager::ExportColorProfile::AdobeRGB);
    REQUIRE_FALSE(adobe.empty());
    const auto tagged = dir / "adobe.jpg";
    REQUIRE(ImageIO::writeJpeg(tagged, *src, 90, adobe));
    auto builtTagged = buildStandardPreview2(lrc::pathToUtf8(tagged));
    REQUIRE(builtTagged.buf);
    REQUIRE_FALSE(builtTagged.sourceColor == 0);

    auto blob = encodeStandardPreview(*builtTagged.buf,
                                      builtTagged.nativeWidth,
                                      builtTagged.nativeHeight,
                                      builtTagged.sourceColor);
    auto decoded = decodeStandardPreview(blob);
    REQUIRE(decoded.buf);
    REQUIRE(decoded.sourceColor == builtTagged.sourceColor);
    REQUIRE(decoded.colorVersion == kStandardPreviewColorVersion);

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
