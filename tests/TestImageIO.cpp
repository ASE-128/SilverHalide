#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/ColorManager.h"
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

// --- R-08: ICC-bearing JPEGs must stay structurally valid and decodable ------
//
// writeJpeg() with a profile encodes to memory, then re-assembles the file as
// "SOI + kept segments + ICC APP2 markers + tail". The tail must begin AT the SOS
// marker. When that offset pointed one marker pair PAST it, the output kept the
// whole ICC payload — so signature-only assertions ("does the file contain
// ICC_PROFILE?") stayed green — but lost its start-of-scan marker. Strict decoders
// reject such a file; stb_image is permissive enough to "decode" it into a
// partially written buffer whose remaining bytes are uninitialised heap memory,
// which is why the same bytes yielded a different image on every decode.
//
// These tests pin the two invariants that defect violated: the file walks as a
// valid JPEG up to its SOS marker, and decoding it reproduces the written pixels
// deterministically.

namespace {

// Test-side JPEG structure walk: steps over SOI and each length-prefixed segment
// and returns the offset of the SOS marker, or npos when the stream is malformed
// or ends first. Deliberately independent of the engine's parser so a future
// regression there cannot make the check agree with it.
size_t strictSosOffset(const std::vector<uint8_t>& b) {
    const size_t npos = std::string::npos;
    if (b.size() < 2 || b[0] != 0xFF || b[1] != 0xD8)
        return npos;
    size_t i = 2;
    while (i + 1 < b.size()) {
        if (b[i] != 0xFF)
            return npos;  // not sitting on a marker boundary
        const uint8_t m = b[i + 1];
        if (m == 0xD9)
            return npos;  // EOI before any scan
        if (m == 0xDA)
            return i;     // Start Of Scan
        if (i + 4 > b.size())
            return npos;
        const size_t len = (static_cast<size_t>(b[i + 2]) << 8) | b[i + 3];
        if (len < 2 || i + 2 + len > b.size())
            return npos;
        i += 2 + len;
    }
    return npos;
}

// First offset at which `needle` occurs in `hay`, or npos.
size_t findBytes(const std::vector<uint8_t>& hay, const std::string& needle) {
    if (needle.empty() || hay.size() < needle.size())
        return std::string::npos;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (std::memcmp(hay.data() + i, needle.data(), needle.size()) == 0)
            return i;
    return std::string::npos;
}

std::vector<uint8_t> readAllBytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

// Payload length of every ICC_PROFILE APP2 chunk, in file order. Walks the
// markers itself and stops at the scan/end-of-image marker.
std::vector<size_t> jpegIccChunkSizes(const std::vector<uint8_t>& b) {
    std::vector<size_t> sizes;
    size_t i = 2;
    while (i + 1 < b.size()) {
        if (b[i] != 0xFF)
            break;
        const uint8_t m = b[i + 1];
        if (m == 0xDA || m == 0xD9)
            break;
        if (i + 4 > b.size())
            break;
        const size_t len = (static_cast<size_t>(b[i + 2]) << 8) | b[i + 3];
        if (len < 2 || i + 2 + len > b.size())
            break;
        if (m == 0xE2 && len >= 16 &&
            std::memcmp(b.data() + i + 4, "ICC_PROFILE", 11) == 0)
            sizes.push_back(len - 16);  // payload minus the 14-byte ICC header
        i += 2 + len;
    }
    return sizes;
}

// A flat patch of an exact byte value. A constant colour survives a q95 JPEG
// round-trip to within a couple of LSBs, which makes a pixel-content assertion
// meaningful without depending on the encoder's exact rounding.
ImageBufferPtr makeFlatU8(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    auto buf = ImageBuffer::create(w, h, PixelFormat::U8);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            auto* px = buf->data().data() + (static_cast<size_t>(y) * w + x) * 3;
            px[0] = r;
            px[1] = g;
            px[2] = b;
        }
    return buf;
}

// Per-channel comparison in 0..1 display space with a stated LSB tolerance.
bool nearlyEqual(const ImageBuffer& img, int x, int y, uint8_t r, uint8_t g,
                 uint8_t b, float tol) {
    return std::fabs(img.pixel(x, y, 0) - r / 255.0f) <= tol &&
           std::fabs(img.pixel(x, y, 1) - g / 255.0f) <= tol &&
           std::fabs(img.pixel(x, y, 2) - b / 255.0f) <= tol;
}

} // namespace

TEST_CASE("ImageIO: R-08 ICC JPEG keeps its SOS marker and decodes back",
          "[io][r08][icc]") {
    auto src = makeFlatU8(24, 16, 60, 160, 100);
    // 568 bytes -> three APP2 chunks, i.e. the multi-segment reassembly path.
    const auto icc = ColorManager::exportProfileBytes(
        ColorManager::ExportColorProfile::AdobeRGB);
    REQUIRE_FALSE(icc.empty());

    // The ICC header's first field is the profile's own size in bytes (big
    // endian). Independent parsers reject the profile outright when that field
    // disagrees with the payload length, so the bytes we embed must be
    // self-consistent to start with.
    const uint32_t declared = (static_cast<uint32_t>(icc[0]) << 24) |
                              (static_cast<uint32_t>(icc[1]) << 16) |
                              (static_cast<uint32_t>(icc[2]) << 8) | icc[3];
    INFO("declared=" << declared << " actual=" << icc.size());
    REQUIRE(declared == icc.size());

    const auto path = std::filesystem::temp_directory_path() / "lrc_r08_icc.jpg";
    REQUIRE(ImageIO::writeJpeg(path, *src, 95, icc));
    const auto bytes = readAllBytes(path);

    // SOI, then a segment walk that must actually reach a start-of-scan marker.
    REQUIRE(bytes.size() > 2);
    REQUIRE(bytes[0] == 0xFF);
    REQUIRE(bytes[1] == 0xD8);
    const size_t sos = strictSosOffset(bytes);
    INFO("bytes=" << bytes.size() << " sos=" << sos);
    REQUIRE(sos != std::string::npos);

    // The profile is embedded in the header, ahead of the scan data (never
    // spliced into it).
    const size_t iccAt = findBytes(bytes, "ICC_PROFILE");
    REQUIRE(iccAt != std::string::npos);
    REQUIRE(iccAt < sos);

    // It must travel in a single APP2 chunk. Qt's JPEG reader drops any
    // ICC_PROFILE chunk with a payload of 146 bytes or fewer, so over-chunking a
    // sub-64 kB profile (the old 255-byte split left a 78-byte tail) silently
    // truncates it and the colour-space parser rejects the whole profile.
    const auto chunks = jpegIccChunkSizes(bytes);
    REQUIRE(chunks.size() == 1);
    REQUIRE(chunks[0] == icc.size());

    // The embedded profile still round-trips byte-for-byte across all chunks.
    const auto source = ImageIO::loadSourceImage(path);
    REQUIRE(source.pixels);
    REQUIRE(source.icc == icc);

    // Decode-back: the pixel content survives the splice (q95, flat patch, so
    // only encoder rounding separates input from output).
    auto decoded = ImageIO::loadImage(path);
    REQUIRE(decoded);
    REQUIRE(decoded->width() == 24);
    REQUIRE(decoded->height() == 16);
    REQUIRE(nearlyEqual(*decoded, 0, 0, 60, 160, 100, 4.0f / 255.0f));

    // Determinism: the same file must decode to the same pixels. This is a
    // secondary guard — a defect that leaves stb's output buffer partly
    // unwritten makes the result depend on heap leftovers, which may or may not
    // differ between two decodes in one process, so the structural and pixel
    // checks above are the deterministic ones.
    auto again = ImageIO::loadImage(path);
    REQUIRE(again);
    REQUIRE(again->data() == decoded->data());

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("ImageIO: R-08 the no-ICC JPEG path stays byte-identical and valid",
          "[io][r08][icc]") {
    auto src = makeFlatU8(24, 16, 200, 40, 90);
    const auto path = std::filesystem::temp_directory_path() / "lrc_r08_plain.jpg";
    REQUIRE(ImageIO::writeJpeg(path, *src, 90));
    const auto bytes = readAllBytes(path);
    REQUIRE(bytes.size() > 2);

    // No profile requested: no ICC signature, but a valid JPEG regardless.
    REQUIRE(findBytes(bytes, "ICC_PROFILE") == std::string::npos);
    REQUIRE(strictSosOffset(bytes) != std::string::npos);

    // The empty-ICC branch is documented as the byte-identical legacy path.
    REQUIRE(bytes == ImageIO::encodeJpeg(*src, 90));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("ImageIO: ICC profiles larger than one APP2 marker split evenly",
          "[io][r08][icc]") {
    // 70000 bytes needs two markers. Neither may be a short tail (see the Qt
    // threshold documented in the R-08 test above). The bytes are arbitrary: the
    // writer and the extractor treat a profile as opaque, so this exercises the
    // chunking maths without needing a real 70 kB profile.
    std::vector<uint8_t> big(70000);
    for (size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);

    auto src = makeFlatU8(8, 8, 10, 20, 30);
    const auto path = std::filesystem::temp_directory_path() / "lrc_r08_bigicc.jpg";
    REQUIRE(ImageIO::writeJpeg(path, *src, 90, big));
    const auto bytes = readAllBytes(path);

    const auto chunks = jpegIccChunkSizes(bytes);
    REQUIRE(chunks.size() == 2);
    for (size_t n : chunks)
        REQUIRE(n > 146);  // never a chunk a strict reader would skip
    REQUIRE(chunks[0] + chunks[1] == big.size());

    // A multi-chunk profile must still leave a valid, decodable JPEG behind.
    REQUIRE(strictSosOffset(bytes) != std::string::npos);
    const auto source = ImageIO::loadSourceImage(path);
    REQUIRE(source.pixels);
    REQUIRE(source.icc == big);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("ImageIO: an unrepresentable ICC profile is dropped, not corrupted",
          "[io][r08][icc]") {
    // seq/count are single bytes, so at most 255 markers (255 * 65519 bytes of
    // profile) can describe one. Beyond that the profile cannot be embedded at
    // all; the writer must degrade to a valid JPEG without it rather than emit a
    // count byte that no longer matches the number of markers.
    const std::vector<uint8_t> huge(255u * 65519u + 1u, 0x5A);
    auto src = makeFlatU8(8, 8, 10, 20, 30);
    const auto path = std::filesystem::temp_directory_path() / "lrc_r08_hugeicc.jpg";
    REQUIRE(ImageIO::writeJpeg(path, *src, 90, huge));
    const auto bytes = readAllBytes(path);

    REQUIRE(jpegIccChunkSizes(bytes).empty());
    REQUIRE(strictSosOffset(bytes) != std::string::npos);
    auto decoded = ImageIO::loadImage(path);
    REQUIRE(decoded);
    REQUIRE(nearlyEqual(*decoded, 0, 0, 10, 20, 30, 4.0f / 255.0f));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
