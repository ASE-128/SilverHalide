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
