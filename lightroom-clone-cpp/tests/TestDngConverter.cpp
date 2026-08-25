#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/DngConverter.h"
#include "engine/RawDecoder.h"

using namespace lrc;

namespace {

constexpr int kWidth = 32;
constexpr int kHeight = 32;
constexpr uint16_t kTopLeft = 4096; // 14-bit sensor value at (0,0)
constexpr uint16_t kBlack = 64;     // 14-bit black level
constexpr uint16_t kWhite = 16383;  // 14-bit white level

// Packs 32 14-bit samples into a row, MSB-first (libraw's getbits order).
// 32*14 bits == 448 bits == 56 bytes, so every row ends byte-aligned.
void packRow14(std::ostream& os, const uint16_t* vals) {
    uint64_t acc = 0;
    int nbits = 0;
    for (int x = 0; x < kWidth; ++x) {
        acc = (acc << 14) | (vals[x] & 0x3FFF);
        nbits += 14;
        while (nbits >= 8) {
            os.put(static_cast<char>((acc >> (nbits - 8)) & 0xFF));
            nbits -= 8;
            acc &= nbits ? ((1ULL << nbits) - 1) : 0ULL;
        }
    }
    // nbits == 0 here because kWidth*14 is a multiple of 8.
}

// Builds a minimal uncompressed 14-bit RGGB CFA DNG with a known black/white
// level, identity ColorMatrix1 and AsShotNeutral={1,1,1}. The make/model are a
// real Sony body so libraw's built-in coefficient database populates cam_xyz
// (a synthetic DNG's ColorMatrix would not otherwise reach color.cam_xyz).
std::filesystem::path writeMinimalDNG14(
    std::array<int, 3> asShotNeutral = {1, 1, 1}) {
    struct Entry {
        uint16_t tag;
        uint16_t type;
        uint32_t count;
        uint32_t value; // inline value or byte offset into the extra area
    };
    std::vector<Entry> ifd;
    std::vector<uint8_t> extra;
    auto addShort = [&](uint16_t tag, uint16_t v) {
        ifd.push_back({tag, 3, 1, v});
    };
    auto addLong = [&](uint16_t tag, uint32_t v) {
        ifd.push_back({tag, 4, 1, v});
    };
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

    addLong(256, kWidth);           // ImageWidth
    addLong(257, kHeight);          // ImageLength
    addShort(258, 14);              // BitsPerSample (14-bit sensor data)
    addShort(259, 1);               // Compression = none
    addShort(262, 32803);           // PhotometricInterpretation = CFA
    addLong(277, 1);                // SamplesPerPixel
    addShort(284, 1);               // PlanarConfiguration = chunky
    addShort(278, kHeight);         // RowsPerStrip
    const uint32_t stripOffTag = static_cast<uint32_t>(ifd.size());
    addLong(273, 0);                // StripOffsets (patched below)
    const uint32_t stripCntTag = static_cast<uint32_t>(ifd.size());
    addLong(279, 0);                // StripByteCounts (patched below)

    const uint8_t dngVer[4] = {1, 4, 0, 0};
    addBytes(50706, dngVer, 4);     // DNGVersion
    const uint8_t dngBack[4] = {1, 1, 0, 0};
    addBytes(50707, dngBack, 4);    // DNGBackwardVersion

    const std::vector<uint16_t> dim{2, 2};
    addShorts(33421, dim);          // CFARepeatPatternDim
    const uint8_t pattern[4] = {0, 1, 1, 2}; // R, G, G, B
    addBytes(33422, pattern, 4);    // CFAPattern

    addShorts(50714, {kBlack});     // BlackLevel (single, 14-bit units)
    addLong(50717, kWhite);         // WhiteLevel (14-bit units)
    const std::vector<uint16_t> crop{kWidth, kHeight};
    addShorts(50720, crop);         // DefaultCropSize

    // CalibrationIlluminant1/2 = D65 (21); ColorMatrix1/2 = identity (1/1 on
    // the diagonal, 0/1 elsewhere). LibRaw only populates cam_xyz from DNG
    // tags when BOTH illuminant+colormatrix pairs are present, so a complete
    // pair is required for DngConverter to emit ColorMatrix1.
    addShort(50778, 21); // CalibrationIlluminant1
    addShort(50779, 21); // CalibrationIlluminant2
    auto addColorMatrix = [&](uint16_t tag) {
        const uint32_t off = static_cast<uint32_t>(extra.size());
        const int32_t mat[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        for (int32_t v : mat) {
            for (int b = 0; b < 4; ++b) // numerator
                extra.push_back(static_cast<uint8_t>((v >> (b * 8)) & 0xFF));
            for (int b = 0; b < 4; ++b) // denominator = 1
                extra.push_back(static_cast<uint8_t>((1 >> (b * 8)) & 0xFF));
        }
        ifd.push_back({tag, 10, 9, off});
    };
    addColorMatrix(50721); // ColorMatrix1
    addColorMatrix(50722); // ColorMatrix2

    // AsShotNeutral (three SRATIONALs, each value/1). Default {1,1,1} is
    // neutral; a non-neutral value lets tests drive the camera white balance.
    {
        const uint32_t off = static_cast<uint32_t>(extra.size());
        for (int c = 0; c < 3; ++c) {
            for (int b = 0; b < 4; ++b) // numerator = asShotNeutral[c]
                extra.push_back(static_cast<uint8_t>(
                    (asShotNeutral[c] >> (b * 8)) & 0xFF));
            for (int b = 0; b < 4; ++b) // denominator = 1
                extra.push_back(static_cast<uint8_t>((1 >> (b * 8)) & 0xFF));
        }
        ifd.push_back({50728, 10, 3, off});
    }

    // Known Sony body so libraw's adobe_coeff() supplies cam_xyz.
    const char make[] = "SONY";
    addBytes(271, make, sizeof(make));
    const char model[] = "ILCE-7CM2";
    addBytes(272, model, sizeof(model));

    // --- Assemble file ---
    const uint32_t ifdCount = static_cast<uint32_t>(ifd.size());
    uint32_t cursor = 8 + 2 + ifdCount * 12 + 4;
    const uint32_t extraOff = cursor;
    const uint32_t pixelOff = extraOff + static_cast<uint32_t>(extra.size());

    const uint32_t packedRowBytes = (kWidth * 14 + 7) / 8; // 56
    ifd[stripOffTag].value = pixelOff;
    ifd[stripCntTag].value =
        packedRowBytes * static_cast<uint32_t>(kHeight);

    for (Entry& e : ifd) {
        if (e.type == 1 && e.count > 4)
            e.value += extraOff;
        else if (e.type == 3 && e.count > 2)
            e.value += extraOff;
        else if (e.type == 10)
            e.value += extraOff;
    }

    const auto path =
        std::filesystem::temp_directory_path() / "lrc_test_min14.dng";
    std::ofstream os(path, std::ios::binary);
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
    put32(8);
    put16(static_cast<uint16_t>(ifdCount));
    for (const Entry& e : ifd) {
        put16(e.tag);
        put16(e.type);
        put32(e.count);
        put32(e.value);
    }
    put32(0);
    os.write(reinterpret_cast<const char*>(extra.data()),
             static_cast<std::streamsize>(extra.size()));

    // 14-bit packed pixel data, MSB-first. Top-left sample = kTopLeft.
    for (int y = 0; y < kHeight; ++y) {
        uint16_t rowVals[kWidth];
        for (int x = 0; x < kWidth; ++x)
            rowVals[x] = static_cast<uint16_t>(kTopLeft + y * 100 + x);
        packRow14(os, rowVals);
    }
    os.close();
    return os ? path : std::filesystem::path{};
}

// Returns the inline 32-bit value field for a tag, or -1 if absent/non-inline.
int64_t readTiffField(const std::vector<uint8_t>& b, uint16_t wantTag) {
    if (b.size() < 8 || b[0] != 'I' || b[1] != 'I')
        return -1;
    const auto rd16 = [&](size_t o) {
        return static_cast<uint16_t>(b[o] | (b[o + 1] << 8));
    };
    const auto rd32 = [&](size_t o) {
        return static_cast<uint32_t>(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) |
                                     (b[o + 3] << 24));
    };
    const uint32_t ifdOff = rd32(4);
    if (ifdOff + 2 > b.size())
        return -1;
    const uint16_t n = rd16(ifdOff);
    for (uint16_t i = 0; i < n; ++i) {
        const size_t e = ifdOff + 2 + static_cast<size_t>(i) * 12;
        if (e + 12 > b.size())
            return -1;
        const uint16_t tag = rd16(e);
        const uint16_t type = rd16(e + 2);
        const uint32_t cnt = rd32(e + 4);
        const uint32_t val = rd32(e + 8);
        if (tag == wantTag) {
            // Inline values only: SHORT[1], LONG[1], BYTE[n<=4].
            if (type == 3 && cnt == 1)
                return val & 0xFFFF;
            if (type == 4 && cnt == 1)
                return val;
            if (type == 1 && cnt <= 4)
                return val;
            if (type == 10)
                return val; // SRATIONAL: out-of-line offset >= 0 means present
            return -1;
        }
    }
    return -1;
}

bool hasTag(const std::vector<uint8_t>& b, uint16_t tag) {
    return readTiffField(b, tag) >= 0;
}

bool containsBytes(const std::vector<uint8_t>& b, uint8_t a, uint8_t c) {
    for (size_t i = 0; i + 1 < b.size(); ++i)
        if (b[i] == a && b[i + 1] == c)
            return true;
    return false;
}

std::vector<uint8_t> readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    return bytes;
}

} // namespace

TEST_CASE("DngConverter: converts a raw DNG preserving the mosaic", "[dng]") {
    const auto inDng = writeMinimalDNG14();
    REQUIRE_FALSE(inDng.empty());
    const auto outDng =
        std::filesystem::temp_directory_path() / "lrc_test_converted.dng";
    std::error_code ec;
    std::filesystem::remove(outDng, ec);

    std::string err;
    REQUIRE(DngConverter::convert(inDng, outDng, &err));
    REQUIRE(err.empty()); // message only set on failure
    REQUIRE(std::filesystem::exists(outDng));

    const std::vector<uint8_t> bytes = readAll(outDng);
    REQUIRE(!bytes.empty());

    // DNGVersion tag id 50706 -> little-endian bytes 0x12 0xC6.
    REQUIRE(containsBytes(bytes, 0x12, 0xC6));
    REQUIRE(hasTag(bytes, 50706));
    // ColorMatrix1 id 50721 -> 0x21 0xC6 (present because cam_xyz was set).
    REQUIRE(containsBytes(bytes, 0x21, 0xC6));
    REQUIRE(hasTag(bytes, 50721));
    // PhotometricInterpretation = CFA 32803 -> 0x23 0x80.
    REQUIRE(containsBytes(bytes, 0x23, 0x80));
    REQUIRE(readTiffField(bytes, 262) == 32803);

    // The pixel strip is exactly kWidth*kHeight*2 bytes at the end of the file.
    const int64_t stripOff = readTiffField(bytes, 273);
    const int64_t stripCnt = readTiffField(bytes, 279);
    REQUIRE(stripOff > 0);
    REQUIRE(stripCnt == kWidth * kHeight * 2);
    REQUIRE(bytes.size() == static_cast<size_t>(stripOff + stripCnt));

    // Top-left sample: the 14-bit value 4096, left-shifted to 16-bit.
    const uint16_t topLeft = static_cast<uint16_t>(bytes[stripOff] |
                                                   (bytes[stripOff + 1] << 8));
    REQUIRE(topLeft == (kTopLeft << 2));

    // Levels are scaled into the 16-bit output range too.
    REQUIRE(readTiffField(bytes, 50714) == (kBlack << 2)); // 256
    REQUIRE(readTiffField(bytes, 50717) == (kWhite << 2)); // 65532

    // CFA stays RGGB. LibRaw canonicalizes the CFAPattern's green to G2
    // (code 3) in `filters`, so the emitted pattern may be {0,1,3,2} instead
    // of the source's {0,1,1,2} - both are valid RGGB. Check the arrangement
    // (R at 0,0; G/G2 across the green row; B at 1,1) rather than exact bytes.
    {
        const int64_t cfaVal = readTiffField(bytes, 33422);
        const int g0 = static_cast<int>(cfaVal & 0xFF);          // row0 col0
        const int g1 = static_cast<int>((cfaVal >> 8) & 0xFF);   // row0 col1
        const int g2 = static_cast<int>((cfaVal >> 16) & 0xFF);  // row1 col0
        const int g3 = static_cast<int>((cfaVal >> 24) & 0xFF);  // row1 col1
        REQUIRE(g0 == 0);                             // R
        const bool greens = (g1 == 1 || g1 == 3) && (g2 == 1 || g2 == 3);
        REQUIRE(greens);                              // G or G2
        REQUIRE(g3 == 2);                             // B
    }

    std::filesystem::remove(inDng, ec);
    std::filesystem::remove(outDng, ec);
}

TEST_CASE("DngConverter: converted DNG re-decodes through RawDecoder", "[dng]") {
    const auto inDng = writeMinimalDNG14();
    REQUIRE_FALSE(inDng.empty());
    const auto outDng =
        std::filesystem::temp_directory_path() / "lrc_test_roundtrip.dng";
    std::error_code ec;
    std::filesystem::remove(outDng, ec);

    std::string err;
    REQUIRE(DngConverter::convert(inDng, outDng, &err));

    // The DNG we write must be loadable by the same decoder the app uses for
    // previews; otherwise "Import as DNG" produces files that fail instantly
    // when the user opens them.
    RawDecoder decoder;
    DecodeParams params;
    params.maxDim = 64;
    auto decoded = decoder.decode(outDng, params);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->linear);
    REQUIRE(decoded->linear->width() > 0);
    REQUIRE(decoded->linear->height() > 0);

    std::filesystem::remove(inDng, ec);
    std::filesystem::remove(outDng, ec);
}

TEST_CASE("DngConverter: rejects a non-raw file", "[dng]") {
    const auto bad = std::filesystem::temp_directory_path() /
                     "lrc_test_notraw.txt";
    {
        std::ofstream o(bad);
        o << "not a raw file";
    }
    const auto outDng =
        std::filesystem::temp_directory_path() / "lrc_test_bad_out.dng";
    std::error_code ec;
    std::filesystem::remove(outDng, ec);

    std::string err;
    REQUIRE_FALSE(DngConverter::convert(bad, outDng, &err));
    REQUIRE_FALSE(err.empty());

    std::filesystem::remove(bad, ec);
    std::filesystem::remove(outDng, ec);
}

TEST_CASE("DngConverter: suggestDngPath never clobbers an existing file",
          "[dng]") {
    const auto raw = std::filesystem::temp_directory_path() /
                     "lrc_suggest_probe.arw";
    const auto first = DngConverter::suggestDngPath(raw);
    std::error_code ec;
    std::filesystem::remove(first, ec); // clean slate for the probe
    REQUIRE(first != raw);              // output differs by extension
    REQUIRE(first.stem() == "lrc_suggest_probe");
    REQUIRE(first.extension() == ".dng");

    // Occupying the first candidate forces a "-2" suffix.
    {
        std::ofstream o(first, std::ios::binary);
        o << "x";
    }
    const auto second = DngConverter::suggestDngPath(raw);
    REQUIRE(second != first);
    REQUIRE(second.extension() == ".dng");
    REQUIRE(second.stem() == "lrc_suggest_probe-2");

    std::filesystem::remove(first, ec);
    // After the conflict file is gone the base name is usable again.
    const auto again = DngConverter::suggestDngPath(raw);
    REQUIRE(again == first);
}

TEST_CASE("RawDecoder: camera white balance is applied (no green cast)",
          "[dng]") {
    // AsShotNeutral = {1,3,1} says the green plane was recorded 3x brighter
    // than red, so a correct decoder must multiply red by ~3 when the camera WB
    // is applied. Regression for the bug where no_auto_scale=1 skipped libraw's
    // scale_colors() entirely, so the WB gains never ran and every RAW decoded
    // with a green cast (R/B far below G).
    const auto inDng = writeMinimalDNG14({1, 3, 1});
    REQUIRE_FALSE(inDng.empty());
    const auto ratio = [&](bool wb) {
        RawDecoder decoder;
        DecodeParams p;
        p.halfSize = true;
        p.applyCameraWB = wb;
        auto d = decoder.decode(inDng, p);
        REQUIRE(d.has_value());
        REQUIRE(d->linear);
        const auto& lin = *d->linear;
        const uint16_t* px =
            reinterpret_cast<const uint16_t*>(lin.data().data());
        const size_t n = static_cast<size_t>(lin.width()) * lin.height();
        double r = 0, g = 0;
        size_t cnt = 0;
        for (size_t i = 0; i < n; i += 16) {
            r += px[i * 3];
            g += px[i * 3 + 1];
            ++cnt;
        }
        return (r / static_cast<double>(cnt)) /
               std::max(g / static_cast<double>(cnt), 1e-6);
    };
    const double r0 = ratio(false);
    const double r1 = ratio(true);
    INFO("R/G with WB off=" << r0 << " with WB on=" << r1);
    // Applying the camera WB must lift red well above the WB-off baseline.
    REQUIRE(r1 > r0 * 1.5);
    std::error_code ec;
    std::filesystem::remove(inDng, ec);
}

