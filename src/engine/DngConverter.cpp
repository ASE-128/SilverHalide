#include "engine/DngConverter.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <libraw/libraw.h>

#include "utils/MathUtils.h"

namespace lrc {

namespace {

// Raw bit depth (12/14/16 for Sony). libraw reports it in
// imgdata.color.raw_bps (set from tiff_bps in identify()); when that is not
// populated, derive the depth from the white level (maximum).
int rawBitDepth(const LibRaw& raw) {
    const int b = static_cast<int>(raw.imgdata.color.raw_bps);
    if (b > 0 && b <= 16)
        return b;
    uint32_t mx = raw.imgdata.color.maximum;
    if (mx == 0)
        mx = 65535;
    // `maximum` can be garbage on corrupted files (>= 2^31). Shifting a 32-bit
    // value by its width is UB; on MSVC the shift count is masked, so
    // (1u << 32) == 1 and the loop never terminates. Bound the loop at bit 31
    // with a 64-bit comparison; anything above 16 bits is clamped below anyway.
    int bits = 1;
    while (bits < 32 && (static_cast<uint64_t>(1) << bits) < mx)
        ++bits;
    if (bits > 16)
        bits = 16;
    return bits;
}

// One SRATIONAL value, stored as little-endian int32 num/den.
struct Srat {
    int32_t num;
    int32_t den;
};

// DNG SRATIONALs use denominator 10000; round to the nearest numerator.
Srat toSrat(double v) {
    return Srat{static_cast<int32_t>(std::llround(v * 10000.0)), 10000};
}

void appendSrat(std::vector<uint8_t>& extra, const Srat& s) {
    const uint32_t n = static_cast<uint32_t>(s.num);
    const uint32_t d = static_cast<uint32_t>(s.den);
    extra.push_back(static_cast<uint8_t>(n & 0xFF));
    extra.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
    extra.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
    extra.push_back(static_cast<uint8_t>((n >> 24) & 0xFF));
    extra.push_back(static_cast<uint8_t>(d & 0xFF));
    extra.push_back(static_cast<uint8_t>((d >> 8) & 0xFF));
    extra.push_back(static_cast<uint8_t>((d >> 16) & 0xFF));
    extra.push_back(static_cast<uint8_t>((d >> 24) & 0xFF));
}

void put16(std::ostream& os, uint16_t v) {
    os.put(static_cast<char>(v & 0xFF));
    os.put(static_cast<char>(v >> 8));
}

void put32(std::ostream& os, uint32_t v) {
    os.put(static_cast<char>(v & 0xFF));
    os.put(static_cast<char>((v >> 8) & 0xFF));
    os.put(static_cast<char>((v >> 16) & 0xFF));
    os.put(static_cast<char>((v >> 24) & 0xFF));
}

} // namespace

bool DngConverter::convert(const std::filesystem::path& inRaw,
                           const std::filesystem::path& outDng,
                           std::string* error) {
    const auto fail = [&](const std::string& msg) {
        if (error)
            *error = msg;
        return false;
    };

    // --- decode the raw mosaic (metadata only; no rendering) --------------
    LibRaw raw;
    const int openRet = openRawFile(raw, inRaw);
    if (openRet != LIBRAW_SUCCESS) {
        return fail("cannot open raw file (libraw error " +
                    std::to_string(openRet) + ")");
    }
    const int unpackRet = raw.unpack();
    if (unpackRet != LIBRAW_SUCCESS) {
        raw.recycle();
        return fail("cannot unpack raw file (libraw error " +
                    std::to_string(unpackRet) + ")");
    }

    ushort* mosaic = raw.imgdata.rawdata.raw_image;
    if (!mosaic) {
        raw.recycle();
        return fail("no raw mosaic (raw_image is null; not a Bayer raw?)");
    }

    const int top = raw.imgdata.sizes.top_margin;
    const int left = raw.imgdata.sizes.left_margin;
    const int width = raw.imgdata.sizes.iwidth;
    const int height = raw.imgdata.sizes.iheight;
    if (width <= 0 || height <= 0) {
        raw.recycle();
        return fail("invalid active area dimensions");
    }

    const int bits = rawBitDepth(raw);
    if (bits <= 0 || bits > 16) {
        raw.recycle();
        return fail("unsupported raw bit depth (" + std::to_string(bits) + ")");
    }
    const int shift = 16 - bits;
    const uint32_t mask = (bits < 16) ? ((1u << bits) - 1u) : 0xFFFFu;

    // --- CFA pattern --------------------------------------------------------
    // LibRaw packs the 2x2 tile into the low byte of `filters`: bits 0-1
    // (row0,col0), 2-3 (row0,col1), 4-5 (row1,col0), 6-7 (row1,col1).
    // LibRaw's internal codes are 0=R, 1=G, 2=B, 3=G2. Note these do NOT
    // match DNG's CFAPattern codes (0=R, 1=G, 2=B, 3=C, 4=M, 5=Y): libraw
    // stores a second green as 3, but DNG requires both greens to be 1.
    // (This LibRaw keeps the field in `idata`; newer builds moved it to color.)
    const uint32_t filters = raw.imgdata.idata.filters;
    std::array<uint8_t, 4> cfaPattern{0, 0, 0, 0};
    if (filters == 0) {
        raw.recycle();
        return fail("cannot determine CFA pattern (filters == 0)");
    }
    cfaPattern[0] = static_cast<uint8_t>(filters & 3);
    cfaPattern[1] = static_cast<uint8_t>((filters >> 2) & 3);
    cfaPattern[2] = static_cast<uint8_t>((filters >> 4) & 3);
    cfaPattern[3] = static_cast<uint8_t>((filters >> 6) & 3);
    // Writing libraw's G2 code (3) verbatim would store DNG code 3 = Cyan and
    // mark the CFA as four distinct colors. libraw itself then re-reads such
    // a file as a 4-color sensor (colors==4) and, with output_color=0,
    // produces 4-channel output -- which our RawDecoder (3-channel pipeline)
    // cannot consume. Normalizing both greens to code 1 is the standard DNG
    // spelling for a Bayer CFA and keeps the written file 3-channel.
    for (uint8_t& c : cfaPattern)
        if (c == 3)
            c = 1;

    // --- black / white levels (scaled to the 16-bit output range) ----------
    const auto scale16 = [shift](uint32_t v) -> uint16_t {
        const uint32_t s = v << shift;
        return static_cast<uint16_t>(s > 65535u ? 65535u : s);
    };

    std::array<uint32_t, 4> black{0, 0, 0, 0};
    bool uniformBlack = false;
    if (raw.imgdata.color.black > 0) {
        uniformBlack = true;
        black[0] = black[1] = black[2] = black[3] = raw.imgdata.color.black;
    } else {
        for (int c = 0; c < 4; ++c)
            black[static_cast<size_t>(c)] = raw.imgdata.color.cblack[c];
        uniformBlack = black[0] == black[1] && black[0] == black[2] &&
                       black[0] == black[3];
    }
    std::array<uint16_t, 4> black16;
    for (int c = 0; c < 4; ++c)
        black16[static_cast<size_t>(c)] = scale16(black[static_cast<size_t>(c)]);

    uint32_t white = raw.imgdata.color.maximum;
    if (white == 0)
        white = (1u << bits) - 1u;
    const uint16_t white16 = scale16(white);

    // --- camera -> XYZ (D65) -> Bradford -> XYZ (D50) -----------------------
    const float(*camXyz)[3] = raw.imgdata.color.cam_xyz;
    bool camXyzZero = true;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (std::fabs(static_cast<double>(camXyz[i][j])) > 1e-6)
                camXyzZero = false;

    double m50[3][3] = {};
    if (!camXyzZero) {
        // Bradford chromatic adaptation, D65 white -> D50 white.
        static const double bradford[3][3] = {
            {1.0478112, 0.0228866, -0.0501270},
            {0.0295424, 0.9904844, -0.0170491},
            {-0.0092345, 0.0150436, 0.7521316}};
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                for (int k = 0; k < 3; ++k)
                    m50[row][col] +=
                        bradford[row][k] *
                        static_cast<double>(camXyz[k][col]);
    }

    // --- as-shot white balance (inverse of cam_mul, green ~1) ---------------
    const float* camMul = raw.imgdata.color.cam_mul;
    const bool camMulZero = camMul[0] == 0 && camMul[1] == 0 && camMul[2] == 0;
    double asn[3] = {1.0, 1.0, 1.0};
    if (!camMulZero) {
        for (int c = 0; c < 3; ++c)
            asn[c] = camMul[c] > 0 ? (1.0 / static_cast<double>(camMul[c]))
                                   : 1.0;
    }

    // --- make / model as NUL-terminated ASCII --------------------------------
    std::string make(raw.imgdata.idata.make);
    make.push_back('\0');
    std::string model(raw.imgdata.idata.model);
    model.push_back('\0');

    // --- assemble the DNG (TIFF header, out-of-line data, offset patching) ---
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
    // TIFF stores <= 4 bytes inline in the value field; larger blobs go to the
    // extra area with absolute offsets patched later.
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
    auto addSRats = [&](uint16_t tag, const std::vector<Srat>& srats) {
        const uint32_t off = static_cast<uint32_t>(extra.size());
        for (const Srat& s : srats)
            appendSrat(extra, s);
        ifd.push_back({tag, 10, static_cast<uint32_t>(srats.size()), off});
    };

    const uint32_t w = static_cast<uint32_t>(width);
    const uint32_t h = static_cast<uint32_t>(height);

    // Entries are pushed in ascending tag order.
    addLong(256, w);                    // ImageWidth
    addLong(257, h);                    // ImageLength
    addShort(258, 16);                  // BitsPerSample
    addShort(259, 1);                   // Compression = none
    addShort(262, 32803);               // PhotometricInterpretation = CFA
    addBytes(271, make.data(), static_cast<uint32_t>(make.size())); // Make
    addBytes(272, model.data(), static_cast<uint32_t>(model.size())); // Model
    const uint32_t stripOffTag = static_cast<uint32_t>(ifd.size());
    addLong(273, 0);                    // StripOffsets (patched below)
    addShort(277, 1);                   // SamplesPerPixel
    addLong(278, h);                    // RowsPerStrip
    const uint32_t stripCntTag = static_cast<uint32_t>(ifd.size());
    addLong(279, 0);                    // StripByteCounts (patched below)
    addShort(284, 1);                   // PlanarConfiguration = chunky

    const std::vector<uint16_t> cfaDim{2, 2};
    addShorts(33421, cfaDim);           // CFARepeatPatternDim
    addBytes(33422, cfaPattern.data(), 4); // CFAPattern

    const uint8_t dngVer[4] = {1, 4, 0, 0};
    addBytes(50706, dngVer, 4);         // DNGVersion
    const uint8_t dngBack[4] = {1, 1, 0, 0};
    addBytes(50707, dngBack, 4);        // DNGBackwardVersion

    // BlackLevel: single value when uniform, else 4 values in CFA order.
    if (uniformBlack) {
        if (black16[0] != 0)
            addShorts(50714, {black16[0]});
    } else {
        bool any = false;
        for (int c = 0; c < 4; ++c)
            any = any || black16[static_cast<size_t>(c)] != 0;
        if (any) {
            std::vector<uint16_t> bl(4);
            for (int i = 0; i < 4; ++i)
                bl[i] = black16[cfaPattern[static_cast<size_t>(i)]];
            addShorts(50714, bl);
        }
    }
    addLong(50717, white16);            // WhiteLevel
    const std::vector<uint16_t> crop{static_cast<uint16_t>(w),
                                     static_cast<uint16_t>(h)};
    addShorts(50720, crop);             // DefaultCropSize

    if (!camXyzZero) {
        // ColorMatrix1: camera RGB -> XYZ D50 (row-major, 9 SRATIONALs).
        std::vector<Srat> m;
        m.reserve(9);
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                m.push_back(toSrat(m50[row][col]));
        addSRats(50721, m);
    }

    std::vector<Srat> asnSrats;
    asnSrats.reserve(3);
    for (int c = 0; c < 3; ++c)
        asnSrats.push_back(toSrat(asn[c]));
    addSRats(50728, asnSrats);          // AsShotNeutral
    addShort(50778, 21);                // CalibrationIlluminant1 = D65

    // --- layout: header + IFD + extra area + pixel strip ---------------------
    const uint32_t ifdCount = static_cast<uint32_t>(ifd.size());
    uint32_t cursor = 8 + 2 + ifdCount * 12 + 4; // header + count + entries + next
    const uint32_t extraOff = cursor;
    const uint32_t pixelOff = extraOff + static_cast<uint32_t>(extra.size());
    const uint32_t stripBytes = w * h * 2;

    ifd[stripOffTag].value = pixelOff;
    ifd[stripCntTag].value = stripBytes;

    // Patch IFD entries that reference the extra data area: TIFF requires
    // absolute file offsets, but the helpers stored relative offsets.
    for (Entry& e : ifd) {
        if (e.type == 1 && e.count > 4)       // BYTE out-of-line
            e.value += extraOff;
        else if (e.type == 3 && e.count > 2)  // SHORT out-of-line
            e.value += extraOff;
        else if (e.type == 10)                // SRATIONAL always out-of-line
            e.value += extraOff;
    }

    std::ofstream os(outDng, std::ios::binary);
    if (!os) {
        raw.recycle();
        return fail("cannot open output file for writing");
    }

    os.put('I');
    os.put('I');
    put16(os, 42);
    put32(os, 8); // IFD0 right after the header
    put16(os, static_cast<uint16_t>(ifdCount));
    for (const Entry& e : ifd) {
        put16(os, e.tag);
        put16(os, e.type);
        put32(os, e.count);
        put32(os, e.value);
    }
    put32(os, 0); // no next IFD
    os.write(reinterpret_cast<const char*>(extra.data()),
             static_cast<std::streamsize>(extra.size()));
    if (!os) {
        raw.recycle();
        return fail("I/O error while writing the DNG header");
    }

    // --- copy the active mosaic area, scaled to 16 bits ----------------------
    const size_t rawWidth = raw.imgdata.sizes.raw_width;
    const size_t srcStride =
        raw.imgdata.sizes.raw_pitch != 0
            ? static_cast<size_t>(raw.imgdata.sizes.raw_pitch) / 2
            : rawWidth;
    if (srcStride < static_cast<size_t>(width)) {
        raw.recycle();
        return fail("raw row pitch is smaller than the active area width");
    }
    const ushort* srcBase =
        mosaic + static_cast<size_t>(top) * srcStride + static_cast<size_t>(left);

    std::vector<uint8_t> rowBuf(static_cast<size_t>(width) * 2);
    for (int y = 0; y < height; ++y) {
        const ushort* s = srcBase + static_cast<size_t>(y) * srcStride;
        uint8_t* d = rowBuf.data();
        for (int x = 0; x < width; ++x) {
            uint32_t v = s[x];
            if (bits < 16)
                v &= mask;
            const uint16_t out = static_cast<uint16_t>(v << shift);
            *d++ = static_cast<uint8_t>(out & 0xFF);
            *d++ = static_cast<uint8_t>(out >> 8);
        }
        os.write(reinterpret_cast<const char*>(rowBuf.data()),
                 static_cast<std::streamsize>(rowBuf.size()));
        if (!os) {
            raw.recycle();
            return fail("I/O error while writing the pixel data");
        }
    }

    os.close();
    if (!os) {
        raw.recycle();
        return fail("I/O error while finalizing the DNG");
    }

    raw.recycle();
    return true;
}

std::filesystem::path DngConverter::suggestDngPath(
    const std::filesystem::path& inRaw) {
    auto candidate = inRaw;
    candidate.replace_extension(".dng");
    std::error_code ec;
    if (!std::filesystem::exists(candidate, ec))
        return candidate;
    const std::filesystem::path parent = candidate.parent_path();
    const std::string stem = candidate.stem().string();
    for (int i = 2; i < 1000; ++i) {
        const auto alt = parent / (stem + "-" + std::to_string(i) + ".dng");
        if (!std::filesystem::exists(alt, ec))
            return alt;
    }
    return candidate; // 1000 existing copies: give up and reuse the base name.
}

} // namespace lrc
