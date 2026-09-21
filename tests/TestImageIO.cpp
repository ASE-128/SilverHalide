#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/ImageBuffer.h"
#include "engine/ImageIO.h"

using namespace Catch;
using namespace lrc;

namespace {

// A small valid U8 sRGB buffer used as the write source.
ImageBufferPtr makeSource() {
    auto buf = ImageBuffer::create(8, 8, PixelFormat::U8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            buf->setPixel(x, y, 0, static_cast<float>(x) / 7.0f);
            buf->setPixel(x, y, 1, 0.5f);
            buf->setPixel(x, y, 2, static_cast<float>(y) / 7.0f);
        }
    return buf;
}

} // namespace

// --- M19: write failure error paths ----------------------------------------
// ImageIO.cpp:339-405 open the ofstream first and return false on failure.
// These exercise invalid / non-existent / read-only targets and check that no
// partial file is produced and nothing crashes.

TEST_CASE("ImageIO: write to a non-existent directory fails cleanly",
          "[io][m19]") {
    const auto bad =
        std::filesystem::temp_directory_path() / "lrc_no_such_dir_m19" / "out";
    auto src = makeSource();

    REQUIRE_FALSE(ImageIO::writeJpeg(bad / "a.jpg", *src, 90));
    REQUIRE_FALSE(ImageIO::writePng(bad / "a.png", *src));
    REQUIRE_FALSE(ImageIO::writeTiff(bad / "a.tif", *src));
    REQUIRE_FALSE(ImageIO::writePPM(bad / "a.ppm", *src));

    auto u16 = ImageBuffer::create(8, 8, PixelFormat::U16);
    REQUIRE_FALSE(ImageIO::writeTiff(bad / "a16.tif", *u16));
    REQUIRE_FALSE(ImageIO::writeDng(bad / "a.dng", *u16));

    // A failed open must not create the directory or a partial file.
    REQUIRE_FALSE(std::filesystem::exists(bad));
}

TEST_CASE("ImageIO: write to an empty path fails", "[io][m19]") {
    auto src = makeSource();
    REQUIRE_FALSE(ImageIO::writeJpeg(std::filesystem::path(), *src, 90));
    REQUIRE_FALSE(ImageIO::writePng(std::filesystem::path(), *src));
    REQUIRE_FALSE(ImageIO::writeTiff(std::filesystem::path(), *src));
    REQUIRE_FALSE(ImageIO::writePPM(std::filesystem::path(), *src));
}

TEST_CASE("ImageIO: write to a path that is a directory fails", "[io][m19]") {
    const auto tmp = std::filesystem::temp_directory_path();
    auto src = makeSource();
    // Opening an existing directory for writing fails on Windows.
    REQUIRE_FALSE(ImageIO::writeJpeg(tmp, *src, 90));
    REQUIRE_FALSE(ImageIO::writePng(tmp, *src));
    REQUIRE_FALSE(ImageIO::writePPM(tmp, *src));
}

TEST_CASE("ImageIO: write to a read-only target fails without truncation",
          "[io][m19]") {
    const auto tmp = std::filesystem::temp_directory_path();
    const auto p = tmp / "lrc_m19_ro.jpg";
    {
        std::ofstream os(p, std::ios::binary);
        os << "pre-existing content";
    }
    std::error_code permEc;
    std::filesystem::permissions(
        p,
        std::filesystem::perms::owner_read | std::filesystem::perms::group_read |
            std::filesystem::perms::others_read,
        std::filesystem::perm_options::replace, permEc);
    REQUIRE_FALSE(permEc);

    auto src = makeSource();
    REQUIRE_FALSE(ImageIO::writeJpeg(p, *src, 90));

    // The pre-existing file must be untouched (no partial / truncated write).
    {
        std::ifstream in(p, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        REQUIRE(content == "pre-existing content");
    }

    // Restore write access so the file can be cleaned up.
    std::filesystem::permissions(p, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add, permEc);
    std::filesystem::remove(p, permEc);
}

TEST_CASE("ImageIO: write rejects wrong buffer format or empty buffers",
          "[io][m19]") {
    auto f32 = ImageBuffer::create(4, 4, PixelFormat::F32);
    auto empty = ImageBuffer::create(0, 0, PixelFormat::U8);
    const auto tmp = std::filesystem::temp_directory_path();

    // JPEG / PNG / PPM require U8; TIFF accepts U8/U16 only; DNG requires U16.
    REQUIRE_FALSE(ImageIO::writeJpeg(tmp / "x.jpg", *f32, 90));
    REQUIRE_FALSE(ImageIO::writePng(tmp / "x.png", *f32));
    REQUIRE_FALSE(ImageIO::writePPM(tmp / "x.ppm", *f32));
    REQUIRE_FALSE(ImageIO::writeTiff(tmp / "x.tif", *f32));
    REQUIRE_FALSE(ImageIO::writeDng(tmp / "x.dng", *f32));

    // Empty buffers are rejected before any file I/O.
    REQUIRE_FALSE(ImageIO::writeJpeg(tmp / "e.jpg", *empty, 90));
    REQUIRE_FALSE(ImageIO::writePng(tmp / "e.png", *empty));
    REQUIRE_FALSE(ImageIO::writePPM(tmp / "e.ppm", *empty));
    REQUIRE_FALSE(ImageIO::writeTiff(tmp / "e.tif", *empty));
    REQUIRE_FALSE(ImageIO::writeDng(tmp / "e.dng", *empty));
}

TEST_CASE("ImageIO: valid write to a real temp path succeeds", "[io][m19]") {
    // Sanity check: the same writer succeeds when the path is valid, so the
    // failures above are about the path, not a broken encoder.
    auto src = makeSource();
    const auto p =
        std::filesystem::temp_directory_path() / "lrc_m19_ok.png";
    REQUIRE(ImageIO::writePng(p, *src));
    REQUIRE(std::filesystem::exists(p));
    REQUIRE(std::filesystem::file_size(p) > 0);
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

// --- T-2: adaptive rows-per-strip -------------------------------------------
// The TIFF writer must cap each strip at max(64, h/64) rows instead of a single
// whole-image strip, so tall (100MP-class) images keep a bounded strip count
// and therefore a bounded decode memory peak. Small images (h <= 64) must stay
// byte-compatible with the prior single-strip layout.

namespace {
// Returns the entry COUNT of `tag` (e.g. the StripOffsets array length for the
// strip count), or 0 when absent.
uint32_t tiffTagCount(const std::vector<uint8_t>& b, uint16_t tag) {
    if (b.size() < 8)
        return 0;
    const bool le = b[0] == 'I' && b[1] == 'I';
    auto rd16 = [&](size_t off) {
        return static_cast<uint16_t>(le ? (b[off] | (b[off + 1] << 8))
                                        : ((b[off] << 8) | b[off + 1]));
    };
    auto rd32 = [&](size_t off) {
        return le ? static_cast<uint32_t>(b[off]) |
                        (static_cast<uint32_t>(b[off + 1]) << 8) |
                        (static_cast<uint32_t>(b[off + 2]) << 16) |
                        (static_cast<uint32_t>(b[off + 3]) << 24)
                  : (static_cast<uint32_t>(b[off]) << 24) |
                        (static_cast<uint32_t>(b[off + 1]) << 16) |
                        (static_cast<uint32_t>(b[off + 2]) << 8) | b[off + 3];
    };
    const uint32_t ifd = rd32(4);
    if (ifd + 2 > b.size())
        return 0;
    const uint16_t n = rd16(ifd);
    for (uint16_t i = 0; i < n; ++i) {
        const size_t e = static_cast<size_t>(ifd) + 2 + i * 12;
        if (e + 12 > b.size())
            break;
        if (rd16(e) == tag)
            return rd32(e + 4);
    }
    return 0;
}

// Returns the inline VALUE of a count==1 `tag` (e.g. RowsPerStrip).
uint32_t tiffTagValue(const std::vector<uint8_t>& b, uint16_t tag) {
    if (b.size() < 8)
        return 0;
    const bool le = b[0] == 'I' && b[1] == 'I';
    auto rd16 = [&](size_t off) {
        return static_cast<uint16_t>(le ? (b[off] | (b[off + 1] << 8))
                                        : ((b[off] << 8) | b[off + 1]));
    };
    auto rd32 = [&](size_t off) {
        return le ? static_cast<uint32_t>(b[off]) |
                        (static_cast<uint32_t>(b[off + 1]) << 8) |
                        (static_cast<uint32_t>(b[off + 2]) << 16) |
                        (static_cast<uint32_t>(b[off + 3]) << 24)
                  : (static_cast<uint32_t>(b[off]) << 24) |
                        (static_cast<uint32_t>(b[off + 1]) << 16) |
                        (static_cast<uint32_t>(b[off + 2]) << 8) | b[off + 3];
    };
    const uint32_t ifd = rd32(4);
    if (ifd + 2 > b.size())
        return 0;
    const uint16_t n = rd16(ifd);
    for (uint16_t i = 0; i < n; ++i) {
        const size_t e = static_cast<size_t>(ifd) + 2 + i * 12;
        if (e + 12 > b.size())
            break;
        if (rd16(e) == tag && rd32(e + 4) == 1)
            return rd32(e + 8);
    }
    return 0;
}
} // namespace

TEST_CASE("ImageIO: T-2 TIFF writer caps strips at max(64,h/64) rows",
          "[io][t2]") {
    // h = 100 -> rowsPerStrip = max(64, 100/64=2) = 64 -> 2 strips. The
    // previous writer emitted a single whole-image strip for every size.
    const int h = 100, w = 61;
    auto buf = ImageBuffer::create(w, h, PixelFormat::U8);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t v = static_cast<uint8_t>((x * 13 + y * 7) & 0xFF);
            auto* p = buf->data().data() + (static_cast<size_t>(y) * w + x) * 3;
            p[0] = v;
            p[1] = static_cast<uint8_t>(v ^ 0x55);
            p[2] = static_cast<uint8_t>(v ^ 0xAA);
        }

    const auto path = std::filesystem::temp_directory_path() / "lrc_t2_strips.tif";
    REQUIRE(ImageIO::writeTiff(path, *buf));

    std::ifstream fin(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(fin)),
                               std::istreambuf_iterator<char>());
    fin.close();

    // RowsPerStrip (278) must be max(64, ceil(h/64)) = 64; StripOffsets (273)
    // must be an array of 2 entries (multi-strip), not a single inline value.
    REQUIRE(tiffTagValue(bytes, 278) == 64u);
    REQUIRE(tiffTagCount(bytes, 273) == 2u);

    // Round-trip: our own TIFF reader must decode the split strips to the exact
    // source pixels (display-ready U8 load).
    auto loaded = ImageIO::loadImage(path);
    REQUIRE(loaded);
    REQUIRE(loaded->width() == w);
    REQUIRE(loaded->height() == h);
    REQUIRE(loaded->data() == buf->data());

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("ImageIO: T-2 small TIFF stays a single strip", "[io][t2]") {
    // Images no taller than 64 rows have rowsPerStrip capped at 64 but still
    // emit exactly one strip (StripOffsets count == 1) that round-trips; the
    // strip layout is unchanged, only RowsPerStrip is normalised to the cap.
    auto src = makeSource();  // 8x8
    const auto path = std::filesystem::temp_directory_path() / "lrc_t2_small.tif";
    REQUIRE(ImageIO::writeTiff(path, *src));
    std::ifstream fin(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(fin)),
                               std::istreambuf_iterator<char>());
    fin.close();
    REQUIRE(tiffTagCount(bytes, 273) == 1u);  // single strip
    REQUIRE(tiffTagValue(bytes, 278) == 64u); // capped rows-per-strip
    auto loaded = ImageIO::loadImage(path);
    REQUIRE(loaded);
    REQUIRE(loaded->data() == src->data());
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
