#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "catalog/ThumbnailCache.h"
#include "catalog/StandardPreview.h"
#include "engine/ColorManager.h"
#include "engine/ImageBuffer.h"
#include "engine/ImageIO.h"
#include "engine/Pipeline.h"
#include "engine/RawDecoder.h"

using namespace Catch;
using namespace lrc;

TEST_CASE("ThumbnailCache: full-res (1:1) cache builds and round-trips",
          "[thumbnail][fullres]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    RawDecoder decoder;
    decoder.setColorManager(cm); // needed to decode the JPEG source

    // Unique temp dir (timestamp + counter avoids clashes with other tests).
    static size_t counter = 0;
    const auto tmpDir =
        std::filesystem::temp_directory_path() /
        ("lrc_fullres_test_" + std::to_string(++counter) + "_" +
         std::to_string(static_cast<unsigned long>(
             std::chrono::system_clock::now().time_since_epoch().count())));

    std::error_code ec;
    std::filesystem::create_directories(tmpDir, ec);
    const auto srcPath = tmpDir / "src.jpg";

    // A filled 96x64 U8 sRGB source image, written as a JPEG.
    auto img = ImageBuffer::create(96, 64, PixelFormat::U8);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 96; ++x) {
            img->setPixel(x, y, 0, static_cast<float>(x) / 95.0f);
            img->setPixel(x, y, 1, 0.5f);
            img->setPixel(x, y, 2, static_cast<float>(y) / 63.0f);
        }
    REQUIRE(ImageIO::writeJpeg(srcPath, *img, 90));

    ThumbnailCache cache(tmpDir);

    // Not built yet.
    REQUIRE_FALSE(cache.findFullRes(srcPath).has_value());

    // ensureFullRes builds the full-res JPEG.
    auto p = cache.ensureFullRes(srcPath, decoder, pipeline);
    REQUIRE(p.has_value());
    REQUIRE(p->extension() == ".jpg");
    REQUIRE(p->string().find("fullres") != std::string::npos);
    REQUIRE(std::filesystem::exists(*p, ec));

    // JPEG SOI marker (0xFF 0xD8).
    {
        std::ifstream in(*p, std::ios::binary);
        REQUIRE(in.good());
        REQUIRE(in.get() == 0xFF);
        REQUIRE(in.get() == 0xD8);
    }

    // findFullRes returns the same path after building.
    auto p2 = cache.findFullRes(srcPath);
    REQUIRE(p2.has_value());
    REQUIRE(*p2 == *p);

    // The cached JPEG decodes back at the source dimensions.
    auto loaded = ImageIO::loadImage(*p);
    REQUIRE_FALSE(loaded->empty());
    REQUIRE(loaded->width() == 96);
    REQUIRE(loaded->height() == 64);

    // Idempotent: a second ensureFullRes returns the same path.
    auto again = cache.ensureFullRes(srcPath, decoder, pipeline);
    REQUIRE(again.has_value());
    REQUIRE(*again == *p);

    // writeFullRes serializes an already-rendered buffer to the same slot.
    REQUIRE(cache.writeFullRes(srcPath, *img));
    auto p3 = cache.findFullRes(srcPath);
    REQUIRE(p3.has_value());
    REQUIRE(*p3 == *p);

    std::filesystem::remove_all(tmpDir, ec);
}

TEST_CASE("ThumbnailCache: renderThumbJpeg/renderFullResJpeg produce in-memory JPEG bytes",
          "[thumbnail][memory-jpeg]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    RawDecoder decoder;
    decoder.setColorManager(cm); // needed to decode the JPEG source

    static size_t counter = 0;
    const auto tmpDir =
        std::filesystem::temp_directory_path() /
        ("lrc_memjpeg_test_" + std::to_string(++counter) + "_" +
         std::to_string(static_cast<unsigned long>(
             std::chrono::system_clock::now().time_since_epoch().count())));

    std::error_code ec;
    std::filesystem::create_directories(tmpDir, ec);
    const auto srcPath = tmpDir / "src.jpg";

    // A filled 96x64 U8 sRGB source image, written as a JPEG.
    auto img = ImageBuffer::create(96, 64, PixelFormat::U8);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 96; ++x) {
            img->setPixel(x, y, 0, static_cast<float>(x) / 95.0f);
            img->setPixel(x, y, 1, 0.5f);
            img->setPixel(x, y, 2, static_cast<float>(y) / 63.0f);
        }
    REQUIRE(ImageIO::writeJpeg(srcPath, *img, 90));

    ThumbnailCache cache(tmpDir);

    // renderFullResJpeg: valid JPEG that decodes back at the source size.
    auto fr = cache.renderFullResJpeg(srcPath, decoder, pipeline);
    REQUIRE(fr.has_value());
    REQUIRE(fr->bytes.size() >= 2);
    REQUIRE(fr->bytes[0] == 0xFF);
    REQUIRE(fr->bytes[1] == 0xD8);
    auto frDecoded = ImageIO::loadImage(fr->bytes.data(), fr->bytes.size());
    REQUIRE_FALSE(frDecoded->empty());
    REQUIRE(frDecoded->width() == 96);
    REQUIRE(frDecoded->height() == 64);
    REQUIRE(fr->width == 96);
    REQUIRE(fr->height == 64);

    // renderThumbJpeg at targetDim 320: valid JPEG, longest side bounded.
    auto th = cache.renderThumbJpeg(srcPath, 320, decoder, pipeline);
    REQUIRE(th.has_value());
    REQUIRE(th->bytes.size() >= 2);
    REQUIRE(th->bytes[0] == 0xFF);
    REQUIRE(th->bytes[1] == 0xD8);
    auto thDecoded = ImageIO::loadImage(th->bytes.data(), th->bytes.size());
    REQUIRE_FALSE(thDecoded->empty());
    const int longest = std::max(thDecoded->width(), thDecoded->height());
    REQUIRE(longest > 0);
    REQUIRE(longest <= 320);
    REQUIRE(th->width == thDecoded->width());
    REQUIRE(th->height == thDecoded->height());

    // Missing source path -> nullopt.
    auto missing = cache.renderThumbJpeg(tmpDir / "nope.jpg", 320, decoder,
                                         pipeline);
    REQUIRE_FALSE(missing.has_value());

    std::filesystem::remove_all(tmpDir, ec);
}

TEST_CASE("ThumbnailCache: renderStandardPreviewBlob produces a v2 catalog blob",
          "[thumbnail][standard-preview][S5]") {
    static size_t counter = 0;
    const auto tmpDir =
        std::filesystem::temp_directory_path() /
        ("lrc_stdprev_blob_" + std::to_string(++counter) + "_" +
         std::to_string(static_cast<unsigned long>(
             std::chrono::system_clock::now().time_since_epoch().count())));
    std::error_code ec;
    std::filesystem::create_directories(tmpDir, ec);
    const auto srcPath = tmpDir / "src.jpg";

    // 1280x960 source (long edge < 2048 so no downscale).
    auto img = ImageBuffer::create(1280, 960, PixelFormat::U8);
    for (int y = 0; y < 960; ++y)
        for (int x = 0; x < 1280; ++x) {
            img->setPixel(x, y, 0, static_cast<float>(x) / 1279.0f);
            img->setPixel(x, y, 1, 0.5f);
            img->setPixel(x, y, 2, static_cast<float>(y) / 959.0f);
        }
    REQUIRE(ImageIO::writeJpeg(srcPath, *img, 90));

    ThumbnailCache cache(tmpDir);
    auto rendered = cache.renderStandardPreviewBlob(srcPath);
    REQUIRE(rendered.has_value());

    // Blob carries the v3 magic + version (RFC v3 records source colour for
    // cache invalidation, R-10).
    REQUIRE(rendered->blob.size() >= 40u); // v3 header
    REQUIRE(rendered->blob[0] == 'S');
    REQUIRE(rendered->blob[1] == 'H');
    REQUIRE(rendered->blob[2] == 'S');
    REQUIRE(rendered->blob[3] == 'P');
    REQUIRE(rendered->blob[4] == 3u); // version == 3

    // Decode through the public StandardPreview API to confirm round-trip.
    auto decoded = decodeStandardPreview(rendered->blob);
    REQUIRE(decoded.buf);
    REQUIRE(decoded.buf->format() == PixelFormat::F32);
    REQUIRE(decoded.nativeWidth == 1280);
    REQUIRE(decoded.nativeHeight == 960);
    // Preview dimensions reported by the wrapper match the decoded buffer.
    REQUIRE(rendered->previewWidth == decoded.buf->width());
    REQUIRE(rendered->previewHeight == decoded.buf->height());
    REQUIRE(rendered->nativeWidth == 1280);
    REQUIRE(rendered->nativeHeight == 960);

    // Missing source -> nullopt.
    auto missing = cache.renderStandardPreviewBlob(tmpDir / "nope.jpg");
    REQUIRE_FALSE(missing.has_value());

    std::filesystem::remove_all(tmpDir, ec);
}
