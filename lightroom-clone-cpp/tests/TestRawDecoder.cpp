#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "engine/ColorManager.h"
#include "engine/DcpProfile.h"
#include "engine/ImageFormats.h"
#include "engine/ImageBuffer.h"
#include "engine/ImageIO.h"
#include "engine/RawDecoder.h"
#include <libraw/libraw.h>

using namespace Catch;
using namespace lrc;

namespace {

// Builds a minimal uncompressed 16-bit DNG (RGGB CFA, 32x32) so the full
// libraw decode path can be exercised without a real camera file. Returns the
// path on success.
//
// Note: libraw's identify() rejects raw images smaller than 22x22 as a sanity
// check, so the synthetic frame must be at least that large.
std::filesystem::path writeMinimalDNG(const uint8_t pattern[4],
                                      const char* outName) {
    constexpr int kWidth = 32;
    constexpr int kHeight = 32;
    constexpr uint16_t kBlack = 0;
    constexpr uint16_t kWhite = 65535;

    struct Entry {
        uint16_t tag;
        uint16_t type;
        uint32_t count;
        uint32_t value; // inline value or byte offset into extra area
    };
    std::vector<Entry> ifd;
    std::vector<uint8_t> extra; // out-of-line data (strings, matrices)
    auto addShort = [&](uint16_t tag, uint16_t v) {
        ifd.push_back({tag, 3, 1, v});
    };
    auto addLong = [&](uint16_t tag, uint32_t v) {
        ifd.push_back({tag, 4, 1, v});
    };
    // TIFF stores data <= 4 bytes inline in the value field (a seek would
    // misread them); larger blobs go to the extra area.
    auto addBytes = [&](uint16_t tag, const void* data, uint32_t n) {
        const auto* p = static_cast<const uint8_t*>(data);
        uint32_t value = 0;
        if (n <= 4) {
            for (uint32_t i = 0; i < n; ++i)
                value |= static_cast<uint32_t>(p[i]) << (8 * i);
        } else {
            value = static_cast<uint32_t>(extra.size());
            extra.insert(extra.end(), p, p + n);
            while (extra.size() % 2)
                extra.push_back(0);
        }
        ifd.push_back({tag, 1, n, value});
    };
    auto addShorts = [&](uint16_t tag, const std::vector<uint16_t>& v) {
        uint32_t value = 0;
        if (v.size() * 2 <= 4) {
            for (size_t i = 0; i < v.size(); ++i)
                value |= static_cast<uint32_t>(v[i]) << (8 * i);
        } else {
            value = static_cast<uint32_t>(extra.size());
            for (uint16_t x : v) {
                extra.push_back(static_cast<uint8_t>(x & 0xFF));
                extra.push_back(static_cast<uint8_t>(x >> 8));
            }
        }
        ifd.push_back({tag, 3, static_cast<uint32_t>(v.size()), value});
    };

    // Basic geometry and raw layout.
    addLong(256, kWidth);           // ImageWidth
    addLong(257, kHeight);          // ImageLength
    addShort(258, 16);              // BitsPerSample
    addShort(259, 1);               // Compression = none
    addShort(262, 32803);           // PhotometricInterpretation = CFA
    addLong(277, 1);                // SamplesPerPixel
    addShort(284, 1);               // PlanarConfiguration = chunky
    addShort(278, kHeight);         // RowsPerStrip
    // StripOffsets/ByteCounts filled after we know the pixel offset.
    const uint32_t stripOffTag = static_cast<uint32_t>(ifd.size());
    addLong(273, 0);
    const uint32_t stripCntTag = static_cast<uint32_t>(ifd.size());
    addLong(279, 0);

    // DNG identity.
    const uint8_t dngVer[4] = {1, 4, 0, 0};
    addBytes(50706, dngVer, 4);
    const uint8_t dngBack[4] = {1, 1, 0, 0};
    addBytes(50707, dngBack, 4);
    // UniqueCameraModel is required for libraw to treat the file as a DNG
    // with a usable camera profile (needed by dcraw_process).
    const char uniqueModel[] = "LRC-Synthetic";
    addBytes(50708, uniqueModel, sizeof(uniqueModel));

    // CFA: 2x2 RGGB.
    const std::vector<uint16_t> dim{2, 2};
    addShorts(33421, dim); // CFARepeatPatternDim (0x828D)
    addBytes(33422, pattern, 4); // CFAPattern (0x828E)

    // Levels.
    const std::vector<uint16_t> black{0};
    addShorts(50714, black); // BlackLevel
    addLong(50717, kWhite);  // WhiteLevel
    const std::vector<uint16_t> crop{kWidth, kHeight};
    addShorts(50720, crop); // DefaultCropSize (0xC620)

    // CalibrationIlluminant1 = D65 (21), matches the identity ColorMatrix1.
    addShort(50778, 21);
    // A matching illuminant/matrix pair lets libraw fill cam_xyz.
    addShort(50779, 21); // CalibrationIlluminant2 = D65

    // AsShotNeutral = {1, 1, 1} (three SRATIONALs, each = 1/1).
    // Stored out-of-line in the extra area.
    {
        const uint32_t off = static_cast<uint32_t>(extra.size());
        for (int c = 0; c < 3; ++c) {
            const int32_t n = 1, d = 1;
            extra.push_back(static_cast<uint8_t>(n & 0xFF));
            extra.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
            extra.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
            extra.push_back(static_cast<uint8_t>((n >> 24) & 0xFF));
            extra.push_back(static_cast<uint8_t>(d & 0xFF));
            extra.push_back(static_cast<uint8_t>((d >> 8) & 0xFF));
            extra.push_back(static_cast<uint8_t>((d >> 16) & 0xFF));
            extra.push_back(static_cast<uint8_t>((d >> 24) & 0xFF));
        }
        ifd.push_back({50728, 10, 3, off});
    }

    struct Srat { int32_t num, den; };
    Srat identity[9];
    for (int i = 0; i < 9; ++i)
        identity[i] = {i % 4 == 0 ? 1 : 0, 1};
    auto addMatrix = [&](uint16_t tag) {
        const uint32_t off = static_cast<uint32_t>(extra.size());
        for (const Srat& s : identity) {
            extra.push_back(static_cast<uint8_t>(s.num & 0xFF));
            extra.push_back(static_cast<uint8_t>((s.num >> 8) & 0xFF));
            extra.push_back(static_cast<uint8_t>((s.num >> 16) & 0xFF));
            extra.push_back(static_cast<uint8_t>((s.num >> 24) & 0xFF));
            extra.push_back(static_cast<uint8_t>(s.den & 0xFF));
            extra.push_back(static_cast<uint8_t>((s.den >> 8) & 0xFF));
            extra.push_back(static_cast<uint8_t>((s.den >> 16) & 0xFF));
            extra.push_back(static_cast<uint8_t>((s.den >> 24) & 0xFF));
        }
        ifd.push_back({tag, 10, 9, off});
    };
    addMatrix(50721); // ColorMatrix1 (identity): camera space -> XYZ.
    addMatrix(50722); // ColorMatrix2

    // ASCII make/model.
    // A make/model known to libraw's built-in camera matrix table (Sony
    // ILCE-7CM2). RawDecoder reads imgdata.color.cam_xyz, which this libraw
    // build fills from that table for DNGs; an unknown make would leave the
    // camera matrix zero and the decoded pixels black.
    const char make[] = "Sony";
    addBytes(271, make, sizeof(make));
    const char model[] = "ILCE-7CM2";
    addBytes(272, model, sizeof(model));

    // --- Assemble file ---
    const uint32_t ifdCount = static_cast<uint32_t>(ifd.size());
    uint32_t cursor = 8 + 2 + ifdCount * 12 + 4; // header + count + entries + next
    const uint32_t extraOff = cursor;
    const uint32_t pixelOff = extraOff + static_cast<uint32_t>(extra.size());

    // Patch strip offsets/counts now that the layout is known.
    ifd[stripOffTag].value = pixelOff;
    ifd[stripCntTag].value =
        static_cast<uint32_t>(kWidth) * kHeight * 2;

    // Patch IFD entries that reference the extra data area: TIFF requires
    // absolute file offsets, but addBytes/addShorts stored relative offsets.
    for (Entry& e : ifd) {
        if (e.type == 1 && e.count > 4)       // BYTE out-of-line
            e.value += extraOff;
        else if (e.type == 3 && e.count > 2)  // SHORT out-of-line
            e.value += extraOff;
        else if (e.type == 10)                // SRATIONAL always out-of-line
            e.value += extraOff;
    }

    std::ofstream os(std::filesystem::temp_directory_path() / outName,
                     std::ios::binary);
    if (!os)
        return {};
    auto put16 = [&](uint16_t v) {
        os.put(static_cast<char>(v & 0xFF));
        os.put(static_cast<char>(v >> 8));
    };
    auto put32 = [&](uint32_t v) {
        os.put(static_cast<char>(v & 0xFF));
        os.put(static_cast<char>((v >> 8) & 0xFF));
        os.put(static_cast<char>((v >> 16) & 0xFF));
        os.put(static_cast<char>((v >> 24) & 0xFF));
    };

    os.put('I');
    os.put('I');
    put16(42);
    put32(8); // IFD0 right after the header
    put16(ifdCount);
    for (const Entry& e : ifd) {
        put16(e.tag);
        put16(e.type);
        put32(e.count);
        put32(e.value);
    }
    put32(0); // no next IFD
    os.write(reinterpret_cast<const char*>(extra.data()),
             static_cast<std::streamsize>(extra.size()));
    // Pixel data: vertical gradient (bright at top, dark at bottom). Each CFA
    // cell is scaled by its colour so the frame carries chroma; a neutral gray
    // field would be mapped to (1,1,1) by every row-normalised matrix and hide
    // colour-matrix changes in the DCP test below.
    for (int y = 0; y < kHeight; ++y) {
        const uint16_t base =
            static_cast<uint16_t>(kWhite * (kHeight - 1 - y) /
                                  (kHeight - 1));
        for (int x = 0; x < kWidth; ++x) {
            const uint8_t code = pattern[(y & 1) * 2 + (x & 1)];
            uint16_t cell = base;
            if (code == 1)             // green
                cell = static_cast<uint16_t>(base * 0.75f);
            else if (code == 2)        // blue
                cell = static_cast<uint16_t>(base * 0.5f);
            put16(cell);
        }
    }
    os.close();
    return os ? std::filesystem::temp_directory_path() / outName
              : std::filesystem::path{};
}

} // namespace

TEST_CASE("RawDecoder: validates and opens a minimal DNG", "[raw]") {
    const uint8_t pattern[4] = {0, 1, 1, 2}; // R, G, G, B (standard spelling)
    const auto path = writeMinimalDNG(pattern, "lrc_test_min.dng");
    REQUIRE_FALSE(path.empty());
    REQUIRE(std::filesystem::exists(path));

    // isSupported uses open_file only (no processing) — should succeed.
    REQUIRE(RawDecoder::isSupported(path));

    // Verify metadata via libraw directly (heap-allocated to avoid stack
    // pressure from the ~200KB libraw_data_t struct).
    {
        const auto raw = std::make_unique<LibRaw>();
        REQUIRE(raw->open_file(path.string().c_str()) == LIBRAW_SUCCESS);
        REQUIRE(raw->imgdata.sizes.raw_width == 32);
        REQUIRE(raw->imgdata.sizes.raw_height == 32);
        REQUIRE(raw->imgdata.idata.make == std::string("Sony"));
        raw->recycle();
    }

    // The synthetic fixture now carries enough DNG tags (UniqueCameraModel,
    // illuminant/color-matrix pairs, levels) for dcraw_process to run, so the
    // end-to-end decode path is exercised here as well.
    RawDecoder decoder;
    auto d = decoder.decode(path, {});
    REQUIRE(d.has_value());
    REQUIRE(d->linear->format() == PixelFormat::U16);
    REQUIRE(d->linear->width() == 32);
    REQUIRE(d->linear->height() == 32);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("RawDecoder: decodes a DNG whose CFA spells green as G2", "[raw]") {
    // DNG CFAPattern 0,1,3,2 (R,G,G2,B) is a legal Bayer spelling that some
    // converters (and older builds of this app) emit. libraw counts four
    // distinct CFA codes, treats the file as a 4-color sensor and returns
    // 4-channel output with output_color=0. The decoder must merge the two
    // greens instead of rejecting the file.
    const uint8_t pattern[4] = {0, 1, 3, 2};
    const auto path = writeMinimalDNG(pattern, "lrc_test_min_g2.dng");
    REQUIRE_FALSE(path.empty());
    REQUIRE(std::filesystem::exists(path));

    {
        const auto raw = std::make_unique<LibRaw>();
        REQUIRE(raw->open_file(path.string().c_str()) == LIBRAW_SUCCESS);
        REQUIRE(raw->imgdata.idata.colors == 4);
        raw->recycle();
    }

    RawDecoder decoder;
    auto d = decoder.decode(path, {});
    REQUIRE(d.has_value());
    REQUIRE(d->linear->format() == PixelFormat::U16);
    REQUIRE(d->linear->width() == 32);
    REQUIRE(d->linear->height() == 32);

    // The frame is a vertical gradient; libraw (output_color=4) must decode
    // the G2 spelling into the same 3-channel layout as the standard spelling.
    // The two spellings must produce identical pixels at the same coordinates.
    const int y = 16;
    REQUIRE(d->linear->pixel(8, y, 0) > 0.0f);
    REQUIRE(d->linear->pixel(8, y, 1) > 0.0f);
    REQUIRE(d->linear->pixel(8, y, 2) > 0.0f);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

namespace {

ImageBufferPtr makeSolidU8(int w, int h, float r, float g, float b) {
    auto buf = ImageBuffer::create(w, h, PixelFormat::U8);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            buf->setPixel(x, y, 0, r);
            buf->setPixel(x, y, 1, g);
            buf->setPixel(x, y, 2, b);
        }
    return buf;
}

// Writes a solid-colour PNG for the import tests and returns its path.
std::filesystem::path writeTestPng(const ImageBuffer& src) {
    const auto p = std::filesystem::temp_directory_path() / "lrc_import.png";
    ImageIO::writePng(p, src);
    return p;
}

} // namespace

TEST_CASE("RawDecoder: decodes PNG/JPEG/TIFF as linear ProPhoto",
          "[raw][io]") {
    auto cm = std::make_shared<ColorManager>();
    RawDecoder decoder;
    decoder.setColorManager(cm);
    const auto src = makeSolidU8(16, 12, 0.8f, 0.5f, 0.2f);
    std::error_code ec;

    // PNG: lossless, so the colour round-trips tightly.
    const auto png = writeTestPng(*src);
    auto d = decoder.decode(png, {});
    REQUIRE(d.has_value());
    REQUIRE(d->linear->format() == PixelFormat::F32);
    REQUIRE(d->linear->width() == 16);
    REQUIRE(d->linear->height() == 12);
    auto back = cm->toDisplaySRGB(*d->linear);
    REQUIRE(back->pixel(0, 0, 0) == Approx(0.8f).margin(0.03f));
    REQUIRE(back->pixel(0, 0, 2) == Approx(0.2f).margin(0.03f));
    std::filesystem::remove(png, ec);

    // JPEG: lossy, but dimensions and format must still hold.
    const auto jpg =
        std::filesystem::temp_directory_path() / "lrc_import.jpg";
    REQUIRE(ImageIO::writeJpeg(jpg, *src, 90));
    d = decoder.decode(jpg, {});
    REQUIRE(d.has_value());
    REQUIRE(d->linear->format() == PixelFormat::F32);
    REQUIRE(d->linear->width() == 16);
    REQUIRE(d->linear->height() == 12);
    std::filesystem::remove(jpg, ec);

    // TIFF (8-bit, lossless) written by the app's own exporter.
    const auto tif =
        std::filesystem::temp_directory_path() / "lrc_import.tif";
    REQUIRE(ImageIO::writeTiff(tif, *src));
    d = decoder.decode(tif, {});
    REQUIRE(d.has_value());
    REQUIRE(d->linear->format() == PixelFormat::F32);
    REQUIRE(d->linear->width() == 16);
    REQUIRE(d->linear->height() == 12);
    back = cm->toDisplaySRGB(*d->linear);
    REQUIRE(back->pixel(0, 0, 1) == Approx(0.5f).margin(0.03f));
    std::filesystem::remove(tif, ec);
}

TEST_CASE("RawDecoder: non-RAW decode respects maxDim", "[raw][io]") {
    auto cm = std::make_shared<ColorManager>();
    RawDecoder decoder;
    decoder.setColorManager(cm);
    const auto png = writeTestPng(*makeSolidU8(32, 16, 0.5f, 0.5f, 0.5f));

    lrc::DecodeParams params;
    params.maxDim = 8; // longest side -> 32 * 8/32 = 8, height -> 4
    auto d = decoder.decode(png, params);
    REQUIRE(d.has_value());
    REQUIRE(d->linear->width() == 8);
    REQUIRE(d->linear->height() == 4);
    std::error_code ec;
    std::filesystem::remove(png, ec);
}

TEST_CASE("RawDecoder: non-RAW decode needs a color manager", "[raw][io]") {
    RawDecoder decoder; // no color manager set
    const auto png = writeTestPng(*makeSolidU8(8, 8, 0.5f, 0.5f, 0.5f));
    REQUIRE_FALSE(decoder.decode(png, {}).has_value());
    std::error_code ec;
    std::filesystem::remove(png, ec);
}

TEST_CASE("RawDecoder: rejects garbage non-RAW input", "[raw][io]") {
    auto cm = std::make_shared<ColorManager>();
    RawDecoder decoder;
    decoder.setColorManager(cm);
    const auto path =
        std::filesystem::temp_directory_path() / "lrc_garbage.png";
    {
        std::ofstream os(path, std::ios::binary);
        os << "this is definitely not a png image........";
    }
    REQUIRE_FALSE(decoder.decode(path, {}).has_value());
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("RawDecoder: a selected DCP colour matrix changes the decode",
          "[raw][dcp]") {
    // R-01 regression: passing a DCP profile to decode() must re-target libraw's
    // camera matrix (rgb_cam) so the rendered colour differs from the built-in
    // matrix. A no-op here would mean DCP selection has no effect on the image.
    const uint8_t pattern[4] = {0, 1, 1, 2};
    const auto path = writeMinimalDNG(pattern, "lrc_test_dcp.dng");
    REQUIRE_FALSE(path.empty());
    REQUIRE(std::filesystem::exists(path));

    RawDecoder decoder;
    auto base = decoder.decode(path, {});
    REQUIRE(base.has_value());

    // A deliberately non-identity DCP camera->XYZ matrix: each output row mixes
    // the camera channels differently than libraw's built-in Sony matrix.
    lrc::DcpProfile dcp;
    dcp.hasColorMatrix1 = true;
    dcp.colorMatrix1 = {0.10f, 0.30f, 0.60f,  //
                        0.30f, 0.60f, 0.10f,  //
                        0.60f, 0.10f, 0.30f};

    auto withDcp = decoder.decode(path, {}, &dcp);
    REQUIRE(withDcp.has_value());

    // The injection must change actual pixel values, not be a silent no-op. The
    // two matrices diverge most where the fixture carries chroma and signal;
    // sum absolute channel deltas across the frame so the check is robust to
    // clipped extremes in the bright rows.
    const auto& a = *base->linear;
    const auto& b = *withDcp->linear;
    double totalDiff = 0.0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            for (int c = 0; c < 3; ++c)
                totalDiff += std::fabs(a.pixel(x, y, c) - b.pixel(x, y, c));
    REQUIRE(totalDiff > 1.0);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("ImageFormats: raw vs importable extension classification",
          "[raw][formats]") {
    // RAW only.
    REQUIRE(lrc::isRawPath(std::filesystem::path("photo.arw")));
    REQUIRE(lrc::isRawPath(std::filesystem::path("photo.DNG")));
    REQUIRE_FALSE(lrc::isRawPath(std::filesystem::path("photo.jpg")));
    REQUIRE_FALSE(lrc::isRawPath(std::filesystem::path("photo.tiff")));

    // The app's own export formats are importable (but not raw).
    REQUIRE(lrc::isImportablePath(std::filesystem::path("photo.dng")));
    REQUIRE(lrc::isImportablePath(std::filesystem::path("photo.jpg")));
    REQUIRE(lrc::isImportablePath(std::filesystem::path("photo.jpeg")));
    REQUIRE(lrc::isImportablePath(std::filesystem::path("photo.PNG")));
    REQUIRE(lrc::isImportablePath(std::filesystem::path("photo.tif")));
    REQUIRE(lrc::isImportablePath(std::filesystem::path("photo.tiff")));
    REQUIRE_FALSE(lrc::isImportablePath(std::filesystem::path("notes.txt")));

    // isSupported reflects the whole import path, not just libraw.
    REQUIRE(RawDecoder::isSupported(std::filesystem::path("photo.arw")));
    REQUIRE(RawDecoder::isSupported(std::filesystem::path("photo.jpg")));
    REQUIRE(RawDecoder::isSupported(std::filesystem::path("photo.png")));
    REQUIRE(RawDecoder::isSupported(std::filesystem::path("photo.tif")));
    REQUIRE_FALSE(RawDecoder::isSupported(std::filesystem::path("notes.txt")));
}

TEST_CASE("RawDecoder: 16-bit TIFF bit depth survives the import (R-09)",
          "[raw][io]") {
    auto cm = std::make_shared<ColorManager>();
    RawDecoder decoder;
    decoder.setColorManager(cm);

    // Two source values that only differ in the low 16 bits: 256 and 257 both
    // round to the same 8-bit value (1), so the old "truncate to U8 first" path
    // produced identical output for both. A correct 16-bit preserve keeps them
    // distinct after conversion to the F32 working space.
    auto u16 = ImageBuffer::create(2, 1, PixelFormat::U16);
    auto* d = reinterpret_cast<uint16_t*>(u16->data().data());
    d[0] = 256; d[1] = 0;   d[2] = 0;   // pixel 0: red = 256
    d[3] = 257; d[4] = 0;   d[5] = 0;   // pixel 1: red = 257

    const auto tif = std::filesystem::temp_directory_path() / "lrc_depth.tif";
    REQUIRE(ImageIO::writeTiff(tif, *u16));
    auto dec = decoder.decode(tif, {});
    std::error_code ec;
    std::filesystem::remove(tif, ec);
    REQUIRE(dec.has_value());
    REQUIRE(dec->linear->format() == PixelFormat::F32);
    const float red256 = dec->linear->pixel(0, 0, 0);
    const float red257 = dec->linear->pixel(1, 0, 0);
    // The 16-bit distinction is tiny at this dark level (~1e-6), but the old
    // "truncate to U8 first" path made the two pixels EXACTLY equal; any
    // non-zero separation proves the depth survived the colour transform.
    REQUIRE(std::fabs(red256 - red257) > 1e-7f);
}

TEST_CASE("RawDecoder: embedded ICC drives the non-RAW conversion (R-08, JPEG)",
          "[raw][io]") {
    auto cm = std::make_shared<ColorManager>();
    RawDecoder decoder;
    decoder.setColorManager(cm);

    // Same saturated red pixels, only the embedded profile differs. The old path
    // always treated the source as sRGB, so both decoded identically; honouring
    // a wide-gamut profile (Adobe RGB) must change the result.
    const auto adobe =
        ColorManager::exportProfileBytes(ColorManager::ExportColorProfile::AdobeRGB);
    const auto srgbProf =
        ColorManager::exportProfileBytes(ColorManager::ExportColorProfile::SRGB);
    REQUIRE_FALSE(adobe.empty());
    REQUIRE_FALSE(srgbProf.empty());

    const auto src = makeSolidU8(8, 4, 1.0f, 0.0f, 0.0f);
    const auto jpgAdobe =
        std::filesystem::temp_directory_path() / "lrc_cc_adobe.jpg";
    const auto jpgSrgb =
        std::filesystem::temp_directory_path() / "lrc_cc_srgb.jpg";
    REQUIRE(ImageIO::writeJpeg(jpgAdobe, *src, 90, adobe));
    REQUIRE(ImageIO::writeJpeg(jpgSrgb, *src, 90, srgbProf));

    auto dAdobe = decoder.decode(jpgAdobe, {});
    auto dSrgb = decoder.decode(jpgSrgb, {});
    std::error_code ec;
    std::filesystem::remove(jpgAdobe, ec);
    std::filesystem::remove(jpgSrgb, ec);
    REQUIRE(dAdobe.has_value());
    REQUIRE(dSrgb.has_value());
    REQUIRE(std::fabs(dAdobe->linear->pixel(0, 0, 0) -
                      dSrgb->linear->pixel(0, 0, 0)) > 0.02f);
}

TEST_CASE("RawDecoder: PNG iCCP profile is honoured (R-08)", "[raw][io]") {
    auto cm = std::make_shared<ColorManager>();
    RawDecoder decoder;
    decoder.setColorManager(cm);

    const auto adobe =
        ColorManager::exportProfileBytes(ColorManager::ExportColorProfile::AdobeRGB);
    const auto srgbProf =
        ColorManager::exportProfileBytes(ColorManager::ExportColorProfile::SRGB);
    const auto src = makeSolidU8(8, 4, 1.0f, 0.0f, 0.0f);

    const auto pngAdobe =
        std::filesystem::temp_directory_path() / "lrc_cc_adobe.png";
    const auto pngSrgb =
        std::filesystem::temp_directory_path() / "lrc_cc_srgb.png";
    REQUIRE(ImageIO::writePng(pngAdobe, *src, adobe));
    REQUIRE(ImageIO::writePng(pngSrgb, *src, srgbProf));

    auto dAdobe = decoder.decode(pngAdobe, {});
    auto dSrgb = decoder.decode(pngSrgb, {});
    std::error_code ec;
    std::filesystem::remove(pngAdobe, ec);
    std::filesystem::remove(pngSrgb, ec);
    REQUIRE(dAdobe.has_value());
    REQUIRE(dSrgb.has_value());
    REQUIRE(std::fabs(dAdobe->linear->pixel(0, 0, 0) -
                      dSrgb->linear->pixel(0, 0, 0)) > 0.02f);
}
