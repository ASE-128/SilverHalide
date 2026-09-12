#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/ColorManager.h"
#include "engine/ExportEngine.h"
#include "engine/ImageBuffer.h"
#include "engine/ImageIO.h"
#include "engine/Pipeline.h"

using namespace Catch;
using namespace lrc;

TEST_CASE("ExportEngine: writes a valid 8-bit sRGB JPEG", "[export]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    ExportEngine exporter(cm, pipeline);

    // A 64x32 F32 linear ProPhoto buffer: left dark, right bright.
    auto linear = ImageBuffer::create(64, 32, PixelFormat::F32);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 64; ++x) {
            const float v = static_cast<float>(x) / 63.0f;
            linear->setPixel(x, y, 0, v);
            linear->setPixel(x, y, 1, v * 0.7f);
            linear->setPixel(x, y, 2, v * 0.4f);
        }
    }

    Adjustments adj;
    adj.exposure = 0.5;
    adj.saturation = 20.0;

    const auto tmp = std::filesystem::temp_directory_path();
    const auto outPath = tmp / "lrc_test_export.jpg";
    REQUIRE(exporter.exportJpeg(outPath, 1, *linear, adj, 90));

    std::vector<uint8_t> bytes;
    {
        std::ifstream in(outPath, std::ios::binary);
        REQUIRE(in.good());
        bytes.assign((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    }
    REQUIRE(bytes.size() > 100);
    // JPEG SOI marker.
    REQUIRE(bytes[0] == 0xFF);
    REQUIRE(bytes[1] == 0xD8);
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
}

TEST_CASE("ExportEngine: PPM thumbnail writer emits a valid P6 header",
          "[export]") {
    auto buf = ImageBuffer::create(4, 3, PixelFormat::U8);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 4; ++x)
            buf->setPixel(x, y, 0, 0.5f);

    const auto tmp = std::filesystem::temp_directory_path();
    const auto outPath = tmp / "lrc_test_thumb.ppm";
    REQUIRE(ExportEngine::writePPM(outPath, *buf));

    std::string head;
    {
        std::ifstream in(outPath, std::ios::binary);
        std::getline(in, head);
    }
    REQUIRE(head == "P6");
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
}

namespace {

ImageBufferPtr makeLinearGradient(int w, int h) {
    auto linear = ImageBuffer::create(w, h, PixelFormat::F32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float v = static_cast<float>(x) / (w - 1);
            linear->setPixel(x, y, 0, v);
            linear->setPixel(x, y, 1, v * 0.7f);
            linear->setPixel(x, y, 2, v * 0.4f);
        }
    }
    return linear;
}

std::vector<uint8_t> readBytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<uint8_t> bytes;
    if (!in)
        return bytes;
    bytes.assign(std::istreambuf_iterator<char>(in),
                 std::istreambuf_iterator<char>());
    return bytes;
}

// Parses the first IFD of a little-endian TIFF into a tag -> value map. Only
// inline values are read, which covers every entry the app's writer emits.
std::unordered_map<uint16_t, uint32_t> parseTiffIfd(
    const std::vector<uint8_t>& bytes) {
    std::unordered_map<uint16_t, uint32_t> out;
    if (bytes.size() < 8 || bytes[0] != 'I' || bytes[1] != 'I')
        return out;
    const uint32_t ifdOff = static_cast<uint32_t>(bytes[4]) |
                            (static_cast<uint32_t>(bytes[5]) << 8) |
                            (static_cast<uint32_t>(bytes[6]) << 16) |
                            (static_cast<uint32_t>(bytes[7]) << 24);
    if (ifdOff + 2 > bytes.size())
        return out;
    const uint16_t count = static_cast<uint16_t>(bytes[ifdOff]) |
                           (static_cast<uint16_t>(bytes[ifdOff + 1]) << 8);
    for (uint16_t i = 0; i < count; ++i) {
        const size_t e = static_cast<size_t>(ifdOff) + 2 + size_t(i) * 12;
        if (e + 12 > bytes.size())
            break;
        const uint16_t tag = static_cast<uint16_t>(bytes[e]) |
                             (static_cast<uint16_t>(bytes[e + 1]) << 8);
        const uint32_t val = static_cast<uint32_t>(bytes[e + 8]) |
                             (static_cast<uint32_t>(bytes[e + 9]) << 8) |
                             (static_cast<uint32_t>(bytes[e + 10]) << 16) |
                             (static_cast<uint32_t>(bytes[e + 11]) << 24);
        out[tag] = val;
    }
    return out;
}

} // namespace

TEST_CASE("ExportEngine: PNG export produces a PNG signature", "[export]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    ExportEngine exporter(cm, pipeline);
    const auto outPath =
        std::filesystem::temp_directory_path() / "lrc_test_export.png";
    REQUIRE(exporter.exportPng(outPath, 2, *makeLinearGradient(32, 16),
                               Adjustments::defaults()));
    const auto bytes = readBytes(outPath);
    REQUIRE(bytes.size() >= 8);
    // PNG magic: 89 50 4E 47 0D 0A 1A 0A
    const uint8_t magic[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    REQUIRE(std::equal(magic, magic + 8, bytes.begin()));
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
}

TEST_CASE("ExportEngine: TIFF export works at 8 and 16 bit", "[export]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    ExportEngine exporter(cm, pipeline);
    const auto linear = makeLinearGradient(32, 16);

    const auto t8 = std::filesystem::temp_directory_path() / "lrc_test_8.tif";
    REQUIRE(exporter.exportTiff(t8, 3, *linear, Adjustments::defaults(), 8));
    const auto b8 = readBytes(t8);
    REQUIRE(b8.size() > 100);
    // Little-endian TIFF magic: "II" then 42.
    REQUIRE(b8[0] == 'I');
    REQUIRE(b8[1] == 'I');
    REQUIRE(b8[2] == 42);
    REQUIRE(b8[3] == 0);
    // The file must be spec-correct enough to re-import: Photometric = RGB,
    // a non-zero StripOffsets pointer and the exact pixel byte count. This
    // guards against the historical off-by-one that wrote Photometric into
    // the StripOffsets slot and left StripOffsets = 0 (unreadable by stb/libraw).
    const auto ifd8 = parseTiffIfd(b8);
    REQUIRE(ifd8.at(262) == 2);                            // Photometric = RGB
    REQUIRE(ifd8.at(273) > 0);                             // StripOffsets
    REQUIRE(ifd8.at(279) == static_cast<uint32_t>(32 * 3 * 16)); // w*3*h
    std::error_code ec;
    std::filesystem::remove(t8, ec);

    const auto t16 = std::filesystem::temp_directory_path() / "lrc_test_16.tif";
    REQUIRE(exporter.exportTiff(t16, 3, *linear, Adjustments::defaults(), 16));
    const auto b16 = readBytes(t16);
    REQUIRE(b16.size() > b8.size()); // 16-bit rows are twice as large
    REQUIRE(b16[0] == 'I');
    REQUIRE(b16[1] == 'I');
    std::filesystem::remove(t16, ec);
}

TEST_CASE("ExportEngine: TIFF rejects invalid bit depths", "[export]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    ExportEngine exporter(cm, pipeline);
    const auto outPath =
        std::filesystem::temp_directory_path() / "lrc_test_bad.tif";
    REQUIRE_FALSE(exporter.exportTiff(outPath, 4, *makeLinearGradient(8, 8),
                                      Adjustments::defaults(), 12));
    REQUIRE_FALSE(exporter.lastError().empty());
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
}

TEST_CASE("ExportEngine: S16 strip-parallel TIFF16 matches single-threaded",
          "[export][S16]") {
    // The strip-parallel quantizeToU16Parallel must produce bit-for-bit
    // identical output to the old serial loop. We verify by exporting a
    // non-trivial gradient twice (once here, once as a reference written by
    // ImageIO::writeTiff from a U16 buffer that we quantize by hand in the
    // old serial way) and comparing the TIFF payload bytes.
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    ExportEngine exporter(cm, pipeline);
    const auto linear = makeLinearGradient(64, 48);

    // S16 export path.
    const auto t16 = std::filesystem::temp_directory_path() / "lrc_s16.tif";
    REQUIRE(exporter.exportTiff(t16, 7, *linear, Adjustments::defaults(), 16));
    const auto b16 = readBytes(t16);
    REQUIRE(b16.size() > 100);
    REQUIRE(b16[0] == 'I');
    REQUIRE(b16[1] == 'I');

    // Reference: serial quantization of the same linear buffer.
    // (Pipeline::renderLinearFull with default adjustments is a no-op on
    // linear ProPhoto, so we can quantize `linear` directly.)
    auto u16 = ImageBuffer::create(linear->width(), linear->height(),
                                   PixelFormat::U16);
    const float* src = reinterpret_cast<const float*>(linear->data().data());
    auto* dst = reinterpret_cast<uint16_t*>(u16->data().data());
    const size_t total = static_cast<size_t>(linear->width()) *
                         linear->height() * 3;
    // Apply the same linear-ProPhoto -> sRGB convert that ExportEngine uses.
    std::vector<float> srgb(total);
    cm->linearProPhotoToSRGB(src, srgb.data(), total / 3);
    for (size_t i = 0; i < total; ++i)
        dst[i] = static_cast<uint16_t>(std::clamp(srgb[i], 0.0f, 1.0f) * 65535.0f);

    const auto tRef = std::filesystem::temp_directory_path() / "lrc_s16_ref.tif";
    REQUIRE(ImageIO::writeTiff(tRef, *u16));
    const auto bRef = readBytes(tRef);

    // The TIFF header bytes are identical (same dimensions, same tags).
    // The pixel payload starts after the 8-byte TIFF header + IFD; we just
    // compare the whole file — any byte difference means the parallel path
    // diverged from the serial reference.
    REQUIRE(b16.size() == bRef.size());
    REQUIRE(b16 == bRef);

    std::error_code ec;
    std::filesystem::remove(t16, ec);
    std::filesystem::remove(tRef, ec);
}

TEST_CASE("ExportEngine: S16 encodeAndWrite dispatches all formats",
          "[export][S16]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    ExportEngine exporter(cm, pipeline);
    const auto linear = makeLinearGradient(16, 12);
    const auto tmp = std::filesystem::temp_directory_path();

    ExportEngine::ExportSpec spec;
    spec.quality = 85;

    std::error_code ec;

    spec.format = ExportEngine::ExportSpec::Format::Jpeg;
    const auto jp = tmp / "lrc_s16_enc.jpg";
    REQUIRE(exporter.encodeAndWrite(jp, 1, *linear, Adjustments::defaults(), spec));
    REQUIRE(std::filesystem::file_size(jp) > 2);
    std::filesystem::remove(jp, ec);

    spec.format = ExportEngine::ExportSpec::Format::Png;
    const auto pn = tmp / "lrc_s16_enc.png";
    REQUIRE(exporter.encodeAndWrite(pn, 2, *linear, Adjustments::defaults(), spec));
    REQUIRE(std::filesystem::file_size(pn) > 8);
    std::filesystem::remove(pn, ec);

    spec.format = ExportEngine::ExportSpec::Format::Tiff8;
    const auto t8 = tmp / "lrc_s16_enc8.tif";
    REQUIRE(exporter.encodeAndWrite(t8, 3, *linear, Adjustments::defaults(), spec));
    REQUIRE(std::filesystem::file_size(t8) > 100);
    std::filesystem::remove(t8, ec);

    spec.format = ExportEngine::ExportSpec::Format::Tiff16;
    const auto t16 = tmp / "lrc_s16_enc16.tif";
    REQUIRE(exporter.encodeAndWrite(t16, 4, *linear, Adjustments::defaults(), spec));
    REQUIRE(std::filesystem::file_size(t16) > 100);
    std::filesystem::remove(t16, ec);

    spec.format = ExportEngine::ExportSpec::Format::Dng;
    const auto dn = tmp / "lrc_s16_enc.dng";
    REQUIRE(exporter.encodeAndWrite(dn, 5, *linear, Adjustments::defaults(), spec));
    REQUIRE(std::filesystem::file_size(dn) > 100);
    std::filesystem::remove(dn, ec);
}

TEST_CASE("ExportEngine: DNG export carries the DNGVersion tag", "[export]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    ExportEngine exporter(cm, pipeline);
    const auto outPath =
        std::filesystem::temp_directory_path() / "lrc_test.dng";
    REQUIRE(exporter.exportDng(outPath, 5, *makeLinearGradient(32, 16),
                               Adjustments::defaults()));
    const auto bytes = readBytes(outPath);
    REQUIRE(bytes.size() > 100);
    // TIFF little-endian header.
    REQUIRE(bytes[0] == 'I');
    REQUIRE(bytes[1] == 'I');
    // DNGVersion tag id 50706 == 0xC612 -> little-endian bytes 12 C6.
    bool found = false;
    for (size_t i = 0; i + 1 < bytes.size(); ++i) {
        if (bytes[i] == 0x12 && bytes[i + 1] == 0xC6) {
            found = true;
            break;
        }
    }
    REQUIRE(found);
    // Same strip/Photometric sanity as the plain TIFF test: a linear DNG with
    // a zero StripOffsets or a corrupted Photometric cannot be re-imported.
    const auto ifd = parseTiffIfd(bytes);
    REQUIRE(ifd.at(262) == 2);                            // Photometric = RGB
    REQUIRE(ifd.at(273) > 0);                             // StripOffsets
    REQUIRE(ifd.at(279) == static_cast<uint32_t>(32 * 3 * 2 * 16)); // w*3*2*h
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
}

TEST_CASE("ExportEngine: DNG never embeds a display ICC profile", "[export][p1]") {
    // R-07 regression: a linear DNG carries linear ProPhoto working-space data
    // that is NOT converted into any display space. Embedding a display ICC
    // (here Adobe RGB) would mislabel those pixels, so the engine must drop the
    // choice even when the spec requests one. The dialog disables ICC for DNG;
    // this asserts the engine honours that contract (InterColorProfile = 34675
    // must be absent from the IFD).
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    ExportEngine exporter(cm, pipeline);

    ExportEngine::ExportSpec spec;
    spec.icc = ExportEngine::ExportSpec::IccChoice::AdobeRGB;

    const auto outPath =
        std::filesystem::temp_directory_path() / "lrc_test_dng_noicc.dng";
    REQUIRE(exporter.exportDng(outPath, 5, *makeLinearGradient(32, 16),
                               Adjustments::defaults(), spec));
    const auto bytes = readBytes(outPath);
    const auto ifd = parseTiffIfd(bytes);
    REQUIRE(ifd.count(34675) == 0);
    // The DNG itself is still valid (Photometric = RGB, strips present).
    REQUIRE(ifd.count(262) == 1);
    REQUIRE(ifd.at(262) == 2);
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
}
TEST_CASE("ImageIO: loadImage decodes a written PNG back", "[export][io]") {
    auto buf = ImageBuffer::create(8, 8, PixelFormat::U8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            buf->setPixel(x, y, 0, static_cast<float>(x) / 7.0f);
            buf->setPixel(x, y, 1, 0.5f);
            buf->setPixel(x, y, 2, static_cast<float>(y) / 7.0f);
        }
    const auto outPath =
        std::filesystem::temp_directory_path() / "lrc_test_roundtrip.png";
    REQUIRE(ImageIO::writePng(outPath, *buf));
    auto loaded = ImageIO::loadImage(outPath);
    REQUIRE_FALSE(loaded->empty());
    REQUIRE(loaded->width() == 8);
    REQUIRE(loaded->height() == 8);
    // PNG is lossless, so the decoded colours match closely.
    REQUIRE(loaded->pixel(7, 0, 0) == Approx(1.0f).margin(0.02f));
    REQUIRE(loaded->pixel(0, 7, 2) == Approx(1.0f).margin(0.02f));
    REQUIRE(loaded->pixel(0, 0, 1) == Approx(0.5f).margin(0.02f));
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
}

TEST_CASE("ImageIO: loadImage decodes written TIFFs back (8 and 16 bit)",
          "[export][io]") {
    auto buf = ImageBuffer::create(8, 8, PixelFormat::U8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            buf->setPixel(x, y, 0, static_cast<float>(x) / 7.0f);
            buf->setPixel(x, y, 1, 0.5f);
            buf->setPixel(x, y, 2, static_cast<float>(y) / 7.0f);
        }
    std::error_code ec;

    const auto t8 =
        std::filesystem::temp_directory_path() / "lrc_test_roundtrip8.tif";
    REQUIRE(ImageIO::writeTiff(t8, *buf));
    auto loaded = ImageIO::loadImage(t8);
    REQUIRE_FALSE(loaded->empty());
    REQUIRE(loaded->width() == 8);
    REQUIRE(loaded->height() == 8);
    // TIFF is lossless for sRGB values at 8-bit.
    REQUIRE(loaded->pixel(7, 0, 0) == Approx(1.0f).margin(0.02f));
    REQUIRE(loaded->pixel(0, 0, 1) == Approx(0.5f).margin(0.02f));
    std::filesystem::remove(t8, ec);

    // 16-bit TIFF: import quantizes to 8-bit but keeps the values.
    auto u16 = ImageBuffer::create(8, 8, PixelFormat::U16);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            u16->setPixel(x, y, 0, static_cast<float>(x) / 7.0f);
            u16->setPixel(x, y, 1, 0.5f);
            u16->setPixel(x, y, 2, static_cast<float>(y) / 7.0f);
        }
    const auto t16 =
        std::filesystem::temp_directory_path() / "lrc_test_roundtrip16.tif";
    REQUIRE(ImageIO::writeTiff(t16, *u16));
    loaded = ImageIO::loadImage(t16);
    REQUIRE_FALSE(loaded->empty());
    REQUIRE(loaded->width() == 8);
    REQUIRE(loaded->height() == 8);
    REQUIRE(loaded->pixel(7, 0, 0) == Approx(1.0f).margin(0.02f));
    REQUIRE(loaded->pixel(0, 0, 1) == Approx(0.5f).margin(0.02f));
    std::filesystem::remove(t16, ec);
}

namespace {

// Writes a minimal *big-endian* uncompressed 8-bit RGB TIFF (single strip)
// and returns whether it succeeded. Guards the built-in reader's BE branches
// (the app's own exports are little-endian, so they never exercise them).
bool writeBeRgbTiff(const std::filesystem::path& path, int w, int h,
                    const std::vector<uint8_t>& rgb) {
    if (w <= 0 || h <= 0 || rgb.size() != static_cast<size_t>(w) * h * 3)
        return false;
    std::ofstream os(path, std::ios::binary);
    if (!os)
        return false;
    const uint32_t pixelOffset = 8 + 2 + 10 * 12 + 4;
    auto be16 = [&](uint16_t v) { os.put(static_cast<char>(v >> 8)); os.put(static_cast<char>(v & 0xFF)); };
    auto be32 = [&](uint32_t v) {
        os.put(static_cast<char>(v >> 24)); os.put(static_cast<char>((v >> 16) & 0xFF));
        os.put(static_cast<char>((v >> 8) & 0xFF)); os.put(static_cast<char>(v & 0xFF));
    };
    auto entry = [&](uint16_t tag, uint16_t type, uint32_t count,
                     uint32_t value) {
        be16(tag);
        be16(type);
        be32(count);
        // SHORT values occupy the first two bytes of the value field, LONG the
        // whole four bytes.
        if (type == 3) {
            be16(static_cast<uint16_t>(value));
            os.put(0);
            os.put(0);
        } else {
            be32(value);
        }
    };
    os.put('M'); os.put('M'); be16(42); be32(8);      // header + IFD offset
    be16(10);                                          // entry count
    entry(256, 4, 1, static_cast<uint32_t>(w));        // ImageWidth
    entry(257, 4, 1, static_cast<uint32_t>(h));        // ImageLength
    entry(258, 3, 1, 8);                               // BitsPerSample
    entry(259, 3, 1, 1);                               // Compression = none
    entry(262, 3, 1, 2);                               // Photometric = RGB
    entry(273, 4, 1, pixelOffset);                     // StripOffsets
    entry(277, 3, 1, 3);                               // SamplesPerPixel
    entry(278, 4, 1, static_cast<uint32_t>(h));        // RowsPerStrip
    entry(279, 4, 1, static_cast<uint32_t>(w) * h * 3); // StripByteCounts
    entry(284, 3, 1, 1);                               // PlanarConfig = chunky
    be32(0);                                           // no next IFD
    os.write(reinterpret_cast<const char*>(rgb.data()),
             static_cast<std::streamsize>(rgb.size()));
    return os.good();
}

} // namespace

TEST_CASE("ImageIO: built-in TIFF reader handles big-endian files",
          "[export][io]") {
    std::vector<uint8_t> rgb;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            rgb.push_back(static_cast<uint8_t>(x * 40));   // R ramps with x
            rgb.push_back(128);                            // G constant
            rgb.push_back(static_cast<uint8_t>(y * 40));   // B ramps with y
        }
    const auto path =
        std::filesystem::temp_directory_path() / "lrc_test_be.tif";
    REQUIRE(writeBeRgbTiff(path, 4, 4, rgb));
    auto loaded = ImageIO::loadImage(path);
    REQUIRE_FALSE(loaded->empty());
    REQUIRE(loaded->width() == 4);
    REQUIRE(loaded->height() == 4);
    REQUIRE(loaded->pixel(3, 0, 0) == Approx(120.0f / 255.0f).margin(0.02f));
    REQUIRE(loaded->pixel(0, 3, 2) == Approx(120.0f / 255.0f).margin(0.02f));
    REQUIRE(loaded->pixel(0, 0, 1) == Approx(128.0f / 255.0f).margin(0.02f));
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("ImageIO: loadImage rejects garbage input", "[export][io]") {
    const auto outPath =
        std::filesystem::temp_directory_path() / "lrc_test_garbage.bin";
    {
        std::ofstream os(outPath, std::ios::binary);
        os << "not an image at all.........";
    }
    auto loaded = ImageIO::loadImage(outPath);
    REQUIRE(loaded->empty());
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
}

TEST_CASE("ColorManager: sRGB round-trips to linear ProPhoto and back",
          "[export][io][color]") {
    auto cm = std::make_shared<ColorManager>();
    auto buf = ImageBuffer::create(4, 4, PixelFormat::U8);
    const float r = 160.0f / 255.0f, g = 100.0f / 255.0f, b = 60.0f / 255.0f;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            buf->setPixel(x, y, 0, r);
            buf->setPixel(x, y, 1, g);
            buf->setPixel(x, y, 2, b);
        }
    auto linear = cm->srgbToLinearProPhoto(*buf);
    REQUIRE(linear->format() == PixelFormat::F32);
    REQUIRE_FALSE(linear->empty());
    auto back = cm->toDisplaySRGB(*linear);
    REQUIRE(back->format() == PixelFormat::U8);
    // Round-trip should come back close to the original sRGB values.
    REQUIRE(back->pixel(0, 0, 0) == Approx(r).margin(0.02f));
    REQUIRE(back->pixel(0, 0, 1) == Approx(g).margin(0.02f));
    REQUIRE(back->pixel(0, 0, 2) == Approx(b).margin(0.02f));
}

TEST_CASE("ExportEngine: export does not pollute the preview LRU",
          "[export][m5]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    ExportEngine exporter(cm, pipeline);

    auto linear = ImageBuffer::create(64, 32, PixelFormat::F32);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 64; ++x) {
            const float v = static_cast<float>(x) / 63.0f;
            linear->setPixel(x, y, 0, v);
            linear->setPixel(x, y, 1, v * 0.7f);
            linear->setPixel(x, y, 2, v * 0.4f);
        }
    Adjustments adj;
    adj.exposure = 0.5;
    adj.saturation = 20.0;

    // Warm the preview LRU with a preview render.
    const auto preview = pipeline.render(1, *linear, adj, 64);
    REQUIRE(preview);
    const size_t before = pipeline.cacheBytes();
    REQUIRE(before > 0);

    // A full-res export must NOT add to the LRU (M5-1: renderFullDisplay is
    // uncached), otherwise a 24 MP export evicts every thumbnail.
    const auto tmp = std::filesystem::temp_directory_path();
    const auto outPath = tmp / "lrc_test_export_m5.jpg";
    REQUIRE(exporter.exportJpeg(outPath, 1, *linear, adj, 90));
    REQUIRE(pipeline.cacheBytes() == before);
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
}

TEST_CASE("ExportEngine: S17-18 encodeAndWrite honours maxDim downscale",
          "[export][S17-18]") {
    // The size preset in ExportDialog routes through encodeAndWrite's maxDim
    // branch: a >0 cap must shrink the longest edge to maxDim (area-average,
    // never upscales) and preserve aspect ratio. A cap >= the native longest
    // edge must be a no-op. We verify by re-importing the lossless PNG and
    // checking the decoded dimensions.
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    ExportEngine exporter(cm, pipeline);
    // 64x48 gradient (4:3, longest edge = 64).
    const auto linear = makeLinearGradient(64, 48);
    const auto tmp = std::filesystem::temp_directory_path();

    ExportEngine::ExportSpec spec;
    spec.format = ExportEngine::ExportSpec::Format::Png;
    spec.quality = 90;

    auto roundTripSize = [&](const std::filesystem::path& p) -> std::pair<int,int> {
        auto img = ImageIO::loadImage(p);
        if (!img || img->empty())
            return {-1, -1};
        return {img->width(), img->height()};
    };

    std::error_code ec;

    // Downscale to longest edge 32 -> 32x24 (preserve 4:3).
    spec.maxDim = 32;
    const auto p32 = tmp / "lrc_s17_32.png";
    REQUIRE(exporter.encodeAndWrite(p32, 1, *linear, Adjustments::defaults(),
                                    spec));
    {
        const auto [w, h] = roundTripSize(p32);
        REQUIRE(w == 32);
        REQUIRE(h == 24);
    }
    std::filesystem::remove(p32, ec);

    // maxDim == native longest edge -> no resize (64x48).
    spec.maxDim = 64;
    const auto p64 = tmp / "lrc_s17_64.png";
    REQUIRE(exporter.encodeAndWrite(p64, 2, *linear, Adjustments::defaults(),
                                    spec));
    {
        const auto [w, h] = roundTripSize(p64);
        REQUIRE(w == 64);
        REQUIRE(h == 48);
    }
    std::filesystem::remove(p64, ec);

    // maxDim > native longest edge -> never upscales (64x48).
    spec.maxDim = 256;
    const auto pBig = tmp / "lrc_s17_big.png";
    REQUIRE(exporter.encodeAndWrite(pBig, 3, *linear, Adjustments::defaults(),
                                    spec));
    {
        const auto [w, h] = roundTripSize(pBig);
        REQUIRE(w == 64);
        REQUIRE(h == 48);
    }
    std::filesystem::remove(pBig, ec);

    // maxDim = 0 -> native resolution (64x48).
    spec.maxDim = 0;
    const auto p0 = tmp / "lrc_s17_0.png";
    REQUIRE(exporter.encodeAndWrite(p0, 4, *linear, Adjustments::defaults(),
                                    spec));
    {
        const auto [w, h] = roundTripSize(p0);
        REQUIRE(w == 64);
        REQUIRE(h == 48);
    }
    std::filesystem::remove(p0, ec);

    // Non-square source: 40x10 gradient, maxDim=20 -> 20x5 (longest=20, the
    // 10px short edge becomes 10 * 20/40 = 5).
    const auto wide = makeLinearGradient(40, 10);
    spec.maxDim = 20;
    const auto pWide = tmp / "lrc_s17_wide.png";
    REQUIRE(exporter.encodeAndWrite(pWide, 5, *wide, Adjustments::defaults(),
                                    spec));
    {
        const auto [w, h] = roundTripSize(pWide);
        REQUIRE(w == 20);
        REQUIRE(h == 5);
    }
    std::filesystem::remove(pWide, ec);
}
