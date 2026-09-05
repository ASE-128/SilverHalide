#include "engine/ImageIO.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// stb_image implementation lives in this TU. STBI_NO_STDIO keeps file access
// through our own std::ifstream so Unicode (e.g. Chinese) paths work.
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace lrc {

namespace {

// ---------------------------------------------------------------------------
// P1-2: ICC profile embedding helpers (JPEG APP2 / PNG iCCP / TIFF tag 34675)
// ---------------------------------------------------------------------------

// Standard PNG CRC-32 (IEEE 802.3 polynomial, reflected MSB-first like zlib).
uint32_t pngCrc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
    return ~crc;
}

void putBe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

// Deflates `data` using a single zlib "stored" block. Valid for input < 64 kB
// (ICC profiles always are). The stream is fully RFC 1950/1951 compliant, so
// any PNG decoder's inflate can read it — no external zlib dependency needed.
std::vector<uint8_t> zlibStoreDeflate(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    if (data.size() >= 65536)  // not encodable in a single stored block
        return {};
    out.push_back(0x78);  // CMF: 32K window, deflate
    out.push_back(0x01);  // FLG: FCHECK=1 -> (0x78*256+0x01) % 31 == 0
    const uint16_t len = static_cast<uint16_t>(data.size());
    const uint16_t nlen = static_cast<uint16_t>(~len);
    out.push_back(0x01);                       // final + stored block
    out.push_back(static_cast<uint8_t>(len & 0xFF));
    out.push_back(static_cast<uint8_t>(len >> 8));
    out.push_back(static_cast<uint8_t>(nlen & 0xFF));
    out.push_back(static_cast<uint8_t>(nlen >> 8));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

// Builds a single PNG chunk: [length][type][data][crc].
std::vector<uint8_t> pngChunk(const char type[5],
                              const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    putBe32(out, static_cast<uint32_t>(data.size()));
    out.insert(out.end(), type, type + 4);
    const size_t base = out.size();
    out.insert(out.end(), data.begin(), data.end());
    const uint32_t crc = pngCrc32(out.data() + base - 4, data.size() + 4);
    putBe32(out, crc);
    return out;
}

// Parses a JPEG byte stream into standalone segment payloads (marker -> bytes,
// in order), excluding the SOS/pixel-data section. Returns (segments, sosOffset)
// where sosOffset is the offset of the FFD9/SOS marker or bytes.size().
// Each entry holds the marker code (e.g. 0x0E for APP14, 0xDA for SOS…).
struct JpegSegment {
    uint8_t marker = 0;              // lower byte, e.g. 0xE0 (APP0), 0xDA (SOS)
    std::vector<uint8_t> payload;    // segment body (excluding the length field)
};

std::vector<JpegSegment> parseJpegSegments(const std::vector<uint8_t>& jpg,
                                           size_t& pixelStart) {
    std::vector<JpegSegment> segs;
    pixelStart = 0;
    const size_t n = jpg.size();
    size_t i = 0;
    // Skip SOI.
    if (n >= 2 && jpg[0] == 0xFF && jpg[1] == 0xD8)
        i = 2;
    else
        return segs;
    while (i + 1 < n) {
        if (jpg[i] != 0xFF) {
            // Ran into entropy-coded data unexpectedly (no SOS seen) — stop.
            break;
        }
        uint8_t marker = jpg[i + 1];
        if (marker == 0xFF) {  // padding byte
            ++i;
            continue;
        }
        i += 2;
        if (marker == 0xDA) {  // Start Of Scan — header ends here
            pixelStart = i;
            break;
        }
        if (marker == 0xD9) {  // EOI with no pixel data
            pixelStart = i;
            break;
        }
        if (i + 2 > n)
            break;
        const size_t segLen = (static_cast<size_t>(jpg[i]) << 8) | jpg[i + 1];
        if (segLen < 2 || i + segLen > n)
            break;
        JpegSegment s;
        s.marker = marker;
        s.payload.assign(jpg.begin() + i + 2, jpg.begin() + i + segLen);
        segs.push_back(std::move(s));
        i += segLen;
    }
    return segs;
}

// Splits an ICC profile into ICC_PROFILE APP2 JPEG markers (max 255 bytes each,
// per the ICC JPEG spec: FFE2, len, "ICC_PROFILE\0", seq, count, data).
std::vector<uint8_t> jpegIccMarkers(const std::vector<uint8_t>& icc) {
    std::vector<uint8_t> out;
    constexpr size_t kMaxData = 255;
    const size_t count = (icc.size() + kMaxData - 1) / kMaxData;
    for (size_t c = 0; c < count; ++c) {
        const size_t begin = c * kMaxData;
        const size_t end = std::min(begin + kMaxData, icc.size());
        const size_t payloadLen = end - begin + 14;  // 12 header + 2 length
        out.push_back(0xFF);
        out.push_back(0xE2);  // APP2
        out.push_back(static_cast<uint8_t>((payloadLen >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(payloadLen & 0xFF));
        const char sig[] = "ICC_PROFILE\0";
        out.insert(out.end(), sig, sig + 12);
        out.push_back(static_cast<uint8_t>(c + 1));
        out.push_back(static_cast<uint8_t>(count));
        out.insert(out.end(), icc.begin() + static_cast<std::ptrdiff_t>(begin),
                   icc.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return out;
}

// Inserts ICC_PROFILE APP2 markers into an encoded JPEG. Existing APP0/APP1/
// APP2/APP14 segments are dropped so the ICC payload is authoritative.
std::vector<uint8_t> embedIccJpeg(const std::vector<uint8_t>& jpg,
                                  const std::vector<uint8_t>& icc) {
    size_t pixelStart = 0;
    auto segs = parseJpegSegments(jpg, pixelStart);
    if (segs.empty())
        return jpg;  // not a parseable JPEG — leave untouched

    std::vector<uint8_t> out;
    out.push_back(0xFF);
    out.push_back(0xD8);  // SOI
    for (const auto& s : segs) {
        // Drop colour-management-adjacent segments to avoid conflicting tags.
        const bool drop = (s.marker == 0xE0 || s.marker == 0xE1 ||
                           s.marker == 0xE2 || s.marker == 0xEE);
        if (drop)
            continue;
        out.push_back(0xFF);
        out.push_back(s.marker);
        const size_t len = s.payload.size() + 2;
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(len & 0xFF));
        out.insert(out.end(), s.payload.begin(), s.payload.end());
    }
    // Insert the ICC chunk before the first pixel-data marker.
    const auto iccMarkers = jpegIccMarkers(icc);
    out.insert(out.end(), iccMarkers.begin(), iccMarkers.end());
    // Aparse the rest of the header + pixel data verbatim.
    out.insert(out.end(), jpg.begin() + static_cast<std::ptrdiff_t>(pixelStart),
               jpg.end());
    return out;
}

std::vector<uint8_t> embedIccPng(const std::vector<uint8_t>& png,
                                 const std::vector<uint8_t>& icc) {
    if (png.size() < 8 || png[0] != 0x89)
        return png;  // not a real PNG — leave untouched
    // iCCP payload: profile-name\0 + compression(0) + zlib-deflated ICC bytes.
    std::vector<uint8_t> data;
    const char name[] = "Silver_Halide";
    data.insert(data.end(), name, name + sizeof(name) - 1);
    data.push_back(0);  // null terminator
    data.push_back(0);  // compression method: zlib
    const auto deflated = zlibStoreDeflate(icc);
    if (deflated.empty())
        return png;
    data.insert(data.end(), deflated.begin(), deflated.end());
    auto iccp = pngChunk("iCCP", data);

    // Re-assemble: signature + IHDR + iCCP + the rest (IDAT/PLTE/IEND…).
    size_t pos = 8;
    std::vector<uint8_t> out;
    out.insert(out.end(), png.begin(), png.begin() + 8);
    // IHDR chunk (first after the signature in a well-formed PNG).
    if (pos + 8 <= png.size() && png[pos + 4] == 'I' && png[pos + 5] == 'H' &&
        png[pos + 6] == 'D' && png[pos + 7] == 'R') {
        const size_t ihdrLen = (static_cast<size_t>(png[pos]) << 24) |
                               (static_cast<size_t>(png[pos + 1]) << 16) |
                               (static_cast<size_t>(png[pos + 2]) << 8) |
                               png[pos + 3];
        const size_t end = pos + 8 + ihdrLen + 4;  // + CRC
        out.insert(out.end(), png.begin() + static_cast<std::ptrdiff_t>(pos),
                   png.begin() + static_cast<std::ptrdiff_t>(std::min(end, png.size())));
        pos = std::min(end, png.size());
    }
    out.insert(out.end(), iccp.begin(), iccp.end());
    out.insert(out.end(), png.begin() + static_cast<std::ptrdiff_t>(pos), png.end());
    return out;
}

struct Sink {
    std::ofstream os;
};

void writeToSink(void* context, void* data, int size) {
    auto* sink = static_cast<Sink*>(context);
    sink->os.write(static_cast<const char*>(data), size);
}

// stb_image_write callback that appends encoded bytes to a std::vector. The
// data pointer is non-const void*, so it must be cast before dereferencing.
void appendToVector(void* context, void* data, int size) {
    auto* vec = static_cast<std::vector<uint8_t>*>(context);
    const auto* bytes = reinterpret_cast<const uint8_t*>(data);
    vec->insert(vec->end(), bytes, bytes + size);
}

// Shared stb_image decode from raw bytes into a display-ready 8-bit sRGB U8
// buffer. Returns an empty buffer on failure.
ImageBufferPtr decodeStbFromMemory(const uint8_t* data, size_t size) {
    auto fail = [] { return ImageBuffer::create(0, 0, PixelFormat::U8); };
    if (!data || size == 0 ||
        size > static_cast<size_t>(std::numeric_limits<int>::max()))
        return fail();
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &n, 3);
    if (!px || w <= 0 || h <= 0)
        return fail();
    auto out = ImageBuffer::create(w, h, PixelFormat::U8);
    std::memcpy(out->data().data(), px, static_cast<size_t>(w) * h * 3);
    stbi_image_free(px);
    return out;
}

bool openBinary(const std::filesystem::path& path, std::ofstream& os) {
    os.open(path, std::ios::binary);
    return os.good();
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

// Core uncompressed little-endian RGB TIFF writer. bps is 8 or 16; `pixels`
// holds width*height*bps/8*3 interleaved bytes. When `dng` is true the DNG
// tags (DNGVersion, LinearRaw, WhiteLevel, ...) are appended, producing a
// linear DNG that libraw round-trips. `icc` (optional) is embedded as tag
// 34675 (InterColorProfile).
bool writeTiffRaw(const std::filesystem::path& path, int w, int h, int bps,
                  const uint8_t* pixels, bool dng = false,
                  const std::vector<uint8_t>* icc = nullptr) {
    if (w <= 0 || h <= 0 || !pixels || (bps != 8 && bps != 16))
        return false;
    std::ofstream os;
    if (!openBinary(path, os))
        return false;

    // IFD entries, ascending by tag. All values fit inline (<= 4 bytes) except
    // the ICC profile, which lives out-of-line at the end of the file.
    struct Entry {
        uint16_t tag, type;
        uint32_t count;
        uint32_t value;  // inline value, or the file offset for the ICC data
        bool isOffset;
    };
    std::vector<Entry> ifd;
    ifd.push_back({256, 4, 1, static_cast<uint32_t>(w), false});          // ImageWidth
    ifd.push_back({257, 4, 1, static_cast<uint32_t>(h), false});          // ImageLength
    ifd.push_back({258, 3, 1, static_cast<uint32_t>(bps), false});        // BitsPerSample
    ifd.push_back({259, 3, 1, 1, false});                                 // Compression = none
    ifd.push_back({262, 3, 1, 2, false});                                 // Photometric = RGB
    ifd.push_back({273, 4, 1, 0, true});                                  // StripOffsets (patched)
    ifd.push_back({277, 3, 1, 3, false});                                 // SamplesPerPixel
    ifd.push_back({278, 4, 1, static_cast<uint32_t>(h), false});          // RowsPerStrip
    ifd.push_back({279, 4, 1, 0, true});                                  // StripByteCounts (patched)
    ifd.push_back({284, 3, 1, 1, false});                                 // PlanarConfig = chunky
    if (dng) {
        // DNGVersion 1.4.0.0 (BYTE[4], packed little-endian: 01 04 00 00).
        ifd.push_back({50706, 1, 4, 0x00000401u, false});
        // DNGBackwardVersion 1.1.0.0.
        ifd.push_back({50707, 1, 4, 0x00000101u, false});
        // WhiteLevel (LONG) = 65535 for a 16-bit linear file.
        ifd.push_back({50717, 4, 1, 65535u, false});
        // LinearRaw = 1 (data is already linear, do not gamma).
        ifd.push_back({50718, 1, 1, 1u, false});
        // CalibrationIlluminant1 = 21 (D65).
        ifd.push_back({50778, 3, 1, 21u, false});
    }
    if (icc && !icc->empty()) {
        // InterColorProfile (34675): an out-of-line byte array of the profile.
        // Reject oversize (TIFF LONG count) profiles defensively.
        if (icc->size() <= 0xFFFFFFFFu)
            ifd.push_back({34675, 1, static_cast<uint32_t>(icc->size()), 0, true});
    }
    // The strip entries sit at fixed offsets even when extra DNG/ICC tags are
    // appended — they are the first two entries with isOffset==true in input
    // order (indices 5 and 8 for the base set). Patch by scanning instead of
    // hard-coding, so adding ICC does not shift them.
    for (auto& e : ifd) {
        if (e.tag == 273) e.value = 0;
        if (e.tag == 279) e.value = 0;
    }

    uint32_t ifdOffset = 8;
    uint32_t ifdBytes =
        2 + static_cast<uint32_t>(ifd.size()) * 12 + 4;
    // Align pixel data to a 4-byte boundary (TIFF-compatible readers like
    // libtiff expect word-aligned strips).
    uint32_t pixelOffset = ifdOffset + ifdBytes;
    const uint32_t rowBytes = static_cast<uint32_t>(w) * 3 * (bps / 8);
    const uint64_t pixelBytes64 = static_cast<uint64_t>(rowBytes) * h;
    uint32_t iccOffset = 0;
    const uint64_t pixelLength = (pixelOffset + pixelBytes64);
    if (icc && !icc->empty()) {
        iccOffset = (pixelLength + 3) & ~3u;
        for (auto& e : ifd)
            if (e.tag == 34675) e.value = iccOffset;  // patched below
    }
    for (auto& e : ifd) {
        if (e.tag == 273) e.value = pixelOffset;
        if (e.tag == 279) e.value = static_cast<uint32_t>(pixelBytes64);
    }

    os.put('I');
    os.put('I');
    put16(os, 42);
    put32(os, ifdOffset);
    put16(os, static_cast<uint16_t>(ifd.size()));
    for (const Entry& e : ifd) {
        put16(os, e.tag);
        put16(os, e.type);
        put32(os, e.count);
        put32(os, e.value);
    }
    put32(os, 0); // no next IFD
    os.write(reinterpret_cast<const char*>(pixels),
             static_cast<std::streamsize>(pixelBytes64));
    // ICC profile bytes (with padding to keep the file 4-byte aligned). This
    // must come after the pixel data because the strip offsets point there.
    if (icc && !icc->empty()) {
        const auto pad = iccOffset - static_cast<uint32_t>(pixelLength);
        for (uint32_t k = 0; k < pad; ++k)
            os.put(0);
        os.write(reinterpret_cast<const char*>(icc->data()),
                 static_cast<std::streamsize>(icc->size()));
    }
    return os.good();
}

// The vendored stb_image has no TIFF support, so TIFF is decoded by this small
// built-in reader. It covers the format the app's own exporter writes and the
// common RGB case other tools produce: uncompressed, chunky, 8/16-bit,
// PhotometricInterpretation = RGB (3 or 4 samples), little- or big-endian,
// single or multiple strips, optional horizontal (Predictor 2) differencing.
// Returns a display-ready 8-bit sRGB buffer, matching the loadImage contract.
ImageBufferPtr loadTiff(const std::vector<uint8_t>& bytes) {
    const size_t n = bytes.size();
    if (n < 8)
        return ImageBuffer::create(0, 0, PixelFormat::U8);
    const bool le = bytes[0] == 'I' && bytes[1] == 'I';
    const bool be = bytes[0] == 'M' && bytes[1] == 'M';
    if (!le && !be)
        return ImageBuffer::create(0, 0, PixelFormat::U8);
    // Magic 42: little-endian stores "42 00", big-endian "00 42".
    if ((le && (bytes[2] != 42 || bytes[3] != 0)) ||
        (be && (bytes[2] != 0 || bytes[3] != 42)))
        return ImageBuffer::create(0, 0, PixelFormat::U8);

    auto rd16 = [&](size_t off) -> uint16_t {
        if (off + 2 > n)
            return 0;
        return le ? static_cast<uint16_t>(bytes[off] |
                                          (bytes[off + 1] << 8))
                  : static_cast<uint16_t>((bytes[off] << 8) | bytes[off + 1]);
    };
    auto rd32 = [&](size_t off) -> uint32_t {
        if (off + 4 > n)
            return 0;
        if (le)
            return static_cast<uint32_t>(bytes[off]) |
                   (static_cast<uint32_t>(bytes[off + 1]) << 8) |
                   (static_cast<uint32_t>(bytes[off + 2]) << 16) |
                   (static_cast<uint32_t>(bytes[off + 3]) << 24);
        return (static_cast<uint32_t>(bytes[off]) << 24) |
               (static_cast<uint32_t>(bytes[off + 1]) << 16) |
               (static_cast<uint32_t>(bytes[off + 2]) << 8) |
               static_cast<uint32_t>(bytes[off + 3]);
    };

    // Walk IFD0, collecting tag -> value list (BYTE/SHORT/LONG, inline or
    // out-of-line).
    const uint32_t ifdOff = rd32(4);
    if (static_cast<size_t>(ifdOff) + 2 > n)
        return ImageBuffer::create(0, 0, PixelFormat::U8);
    const uint16_t entryCount = rd16(ifdOff);
    std::unordered_map<uint16_t, std::vector<uint32_t>> tags;
    for (uint16_t i = 0; i < entryCount; ++i) {
        const size_t e = static_cast<size_t>(ifdOff) + 2 + static_cast<size_t>(i) * 12;
        if (e + 12 > n)
            break;
        const uint16_t tag = rd16(e);
        const uint16_t type = rd16(e + 2);
        const uint32_t count = rd32(e + 4);
        const uint32_t field = rd32(e + 8);
        const uint32_t elemSize = type == 1 ? 1u : (type == 3 ? 2u : (type == 4 ? 4u : 0u));
        if (elemSize == 0)
            continue;
        std::vector<uint32_t> values;
        const uint64_t byteLen = static_cast<uint64_t>(count) * elemSize;
        if (byteLen <= 4) {
            for (uint32_t k = 0; k < count; ++k) {
                const size_t off = e + 8 + static_cast<size_t>(k) * elemSize;
                values.push_back(type == 1 ? bytes[off] : (type == 3 ? rd16(off) : rd32(off)));
            }
        } else {
            if (static_cast<uint64_t>(field) + byteLen > n)
                continue;
            for (uint32_t k = 0; k < count; ++k) {
                const size_t off = static_cast<size_t>(field) +
                                   static_cast<size_t>(k) * elemSize;
                values.push_back(type == 1 ? bytes[off] : (type == 3 ? rd16(off) : rd32(off)));
            }
        }
        tags[tag] = std::move(values);
    }

    auto one = [&](uint16_t tag, uint32_t def) -> uint32_t {
        auto it = tags.find(tag);
        return (it != tags.end() && !it->second.empty()) ? it->second[0] : def;
    };
    auto list = [&](uint16_t tag) -> const std::vector<uint32_t>* {
        auto it = tags.find(tag);
        return it == tags.end() ? nullptr : &it->second;
    };

    const uint32_t w = one(256, 0), h = one(257, 0);
    if (w == 0 || h == 0)
        return ImageBuffer::create(0, 0, PixelFormat::U8);
    const uint32_t bps = one(258, 8);
    const uint32_t compression = one(259, 1);
    const uint32_t photometric = one(262, 0);
    const uint32_t spp = one(277, 3);
    uint32_t rowsPerStrip = one(278, h);
    const uint32_t predictor = one(317, 1);
    const uint32_t planar = one(284, 1);
    // Uncompressed RGB, chunky, 8/16-bit unsigned is what we support. Anything
    // else (compressed, CFA raw/DNG, CMYK, float) falls back to failure so the
    // caller can treat the file as unsupported rather than corrupt.
    if (compression != 1 || planar != 1 || photometric != 2 ||
        (bps != 8 && bps != 16) || (spp != 3 && spp != 4) ||
        (predictor != 1 && predictor != 2))
        return ImageBuffer::create(0, 0, PixelFormat::U8);
    if (rowsPerStrip == 0)
        rowsPerStrip = h;

    const std::vector<uint32_t>* offsets = list(273);
    const std::vector<uint32_t>* counts = list(279);
    if (!offsets || offsets->empty() || !counts || counts->empty())
        return ImageBuffer::create(0, 0, PixelFormat::U8);

    const uint32_t nStrips = (h + rowsPerStrip - 1) / rowsPerStrip;
    auto pick = [](const std::vector<uint32_t>& v, uint32_t i) {
        return v[std::min(i, static_cast<uint32_t>(v.size() - 1))];
    };

    const size_t sampleBytes = bps / 8;
    const size_t bytesPerRow = static_cast<size_t>(w) * spp * sampleBytes;
    auto out = ImageBuffer::create(static_cast<int>(w), static_cast<int>(h),
                                   PixelFormat::U8);
    uint8_t* dst = out->data().data();

    std::vector<uint8_t> row(bytesPerRow);
    // Row-local accessors: the row is a copy of the file's bytes, so reads use
    // its own indices, not the absolute file offsets rd16/rd32 expect.
    auto row16 = [&](size_t off) -> uint16_t {
        return le ? static_cast<uint16_t>(row[off] | (row[off + 1] << 8))
                  : static_cast<uint16_t>((row[off] << 8) | row[off + 1]);
    };
    for (uint32_t s = 0; s < nStrips; ++s) {
        const uint32_t ofs = pick(*offsets, s);
        const uint32_t cnt = pick(*counts, s);
        if (static_cast<size_t>(ofs) + cnt > n)
            return ImageBuffer::create(0, 0, PixelFormat::U8);
        const uint32_t rowsInStrip =
            std::min(rowsPerStrip, h - s * rowsPerStrip);
        for (uint32_t r = 0; r < rowsInStrip; ++r) {
            const size_t rowStart = static_cast<size_t>(ofs) +
                                    static_cast<size_t>(r) * bytesPerRow;
            if (rowStart + bytesPerRow > n)
                return ImageBuffer::create(0, 0, PixelFormat::U8);
            std::memcpy(row.data(), bytes.data() + rowStart, bytesPerRow);
            // Predictor 2: each sample channel is the difference from the
            // same channel one pixel to the left.
            if (predictor == 2) {
                const size_t channelStride = static_cast<size_t>(spp) * sampleBytes;
                for (size_t i = channelStride; i < bytesPerRow;
                     i += sampleBytes) {
                    if (sampleBytes == 1) {
                        row[i] = static_cast<uint8_t>(row[i] + row[i - spp]);
                    } else {
                        const uint16_t prev = row16(i - channelStride);
                        uint16_t cur = row16(i);
                        cur = static_cast<uint16_t>(cur + prev);
                        if (le) {
                            row[i] = static_cast<uint8_t>(cur & 0xFF);
                            row[i + 1] = static_cast<uint8_t>(cur >> 8);
                        } else {
                            row[i] = static_cast<uint8_t>(cur >> 8);
                            row[i + 1] = static_cast<uint8_t>(cur & 0xFF);
                        }
                    }
                }
            }
            const int y = static_cast<int>(s * rowsPerStrip + r);
            for (uint32_t x = 0; x < w; ++x) {
                const size_t p = static_cast<size_t>(x) * spp * sampleBytes;
                uint8_t r8, g8, b8;
                if (sampleBytes == 1) {
                    r8 = row[p];
                    g8 = row[p + 1];
                    b8 = row[p + 2];
                } else {
                    const uint16_t rv = row16(p), gv = row16(p + 2),
                                     bv = row16(p + 4);
                    r8 = static_cast<uint8_t>(
                        (static_cast<uint32_t>(rv) * 255u + 32767u) / 65535u);
                    g8 = static_cast<uint8_t>(
                        (static_cast<uint32_t>(gv) * 255u + 32767u) / 65535u);
                    b8 = static_cast<uint8_t>(
                        (static_cast<uint32_t>(bv) * 255u + 32767u) / 65535u);
                }
                uint8_t* px = dst + (static_cast<size_t>(y) * w + x) * 3;
                px[0] = r8;
                px[1] = g8;
                px[2] = b8;
            }
        }
    }
    return out;
}

} // namespace

bool ImageIO::writeJpeg(const std::filesystem::path& path,
                        const ImageBuffer& rgbU8, int quality,
                        const std::vector<uint8_t>& icc) {
    if (rgbU8.format() != PixelFormat::U8 || rgbU8.empty())
        return false;
    std::ofstream os;
    if (!openBinary(path, os))
        return false;
    if (icc.empty()) {
        // Byte-identical legacy path when no profile is requested.
        Sink sink{std::move(os)};
        const int ok = stbi_write_jpg_to_func(
            writeToSink, &sink, rgbU8.width(), rgbU8.height(), 3,
            rgbU8.data().data(), std::clamp(quality, 0, 100));
        sink.os.flush();
        return ok != 0 && sink.os.good();
    }
    // Encode to memory, splice the ICC_PROFILE APP2 markers, then write.
    std::vector<uint8_t> bytes;
    const int ok = stbi_write_jpg_to_func(
        appendToVector, &bytes, rgbU8.width(), rgbU8.height(), 3,
        rgbU8.data().data(), std::clamp(quality, 0, 100));
    if (ok == 0)
        return false;
    const auto withIcc = embedIccJpeg(bytes, icc);
    os.write(reinterpret_cast<const char*>(withIcc.data()),
             static_cast<std::streamsize>(withIcc.size()));
    os.flush();
    return os.good();
}

bool ImageIO::writePng(const std::filesystem::path& path,
                       const ImageBuffer& rgbU8,
                       const std::vector<uint8_t>& icc) {
    if (rgbU8.format() != PixelFormat::U8 || rgbU8.empty())
        return false;
    std::ofstream os;
    if (!openBinary(path, os))
        return false;
    if (icc.empty()) {
        // Byte-identical legacy path when no profile is requested.
        Sink sink{std::move(os)};
        const int ok = stbi_write_png_to_func(
            writeToSink, &sink, rgbU8.width(), rgbU8.height(), 3,
            rgbU8.data().data(), rgbU8.width() * 3);
        sink.os.flush();
        return ok != 0 && sink.os.good();
    }
    std::vector<uint8_t> bytes;
    const int ok = stbi_write_png_to_func(
        appendToVector, &bytes, rgbU8.width(), rgbU8.height(), 3,
        rgbU8.data().data(), rgbU8.width() * 3);
    if (ok == 0)
        return false;
    const auto withIcc = embedIccPng(bytes, icc);
    os.write(reinterpret_cast<const char*>(withIcc.data()),
             static_cast<std::streamsize>(withIcc.size()));
    os.flush();
    return os.good();
}

bool ImageIO::writeTiff(const std::filesystem::path& path,
                        const ImageBuffer& rgb,
                        const std::vector<uint8_t>& icc) {
    if (rgb.empty())
        return false;
    const std::vector<uint8_t>* iccPtr = icc.empty() ? nullptr : &icc;
    if (rgb.format() == PixelFormat::U8)
        return writeTiffRaw(path, rgb.width(), rgb.height(), 8,
                            rgb.data().data(), false, iccPtr);
    if (rgb.format() == PixelFormat::U16)
        return writeTiffRaw(path, rgb.width(), rgb.height(), 16,
                            rgb.data().data(), false, iccPtr);
    return false;
}

bool ImageIO::writePPM(const std::filesystem::path& path,
                       const ImageBuffer& rgbU8) {
    if (rgbU8.format() != PixelFormat::U8 || rgbU8.empty())
        return false;
    const int w = rgbU8.width();
    const int h = rgbU8.height();
    std::ofstream os;
    if (!openBinary(path, os))
        return false;
    os << "P6\n" << w << ' ' << h << "\n255\n";
    os.write(reinterpret_cast<const char*>(rgbU8.data().data()),
             static_cast<std::streamsize>(rgbU8.sizeBytes()));
    return os.good();
}

bool ImageIO::writeDng(const std::filesystem::path& path,
                       const ImageBuffer& rgbU16) {
    if (rgbU16.format() != PixelFormat::U16 || rgbU16.empty())
        return false;
    return writeTiffRaw(path, rgbU16.width(), rgbU16.height(), 16,
                        rgbU16.data().data(), /*dng=*/true);
}

ImageBufferPtr ImageIO::loadImage(const std::filesystem::path& path) {
    auto fail = [] { return ImageBuffer::create(0, 0, PixelFormat::U8); };
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return fail();
    in.seekg(0, std::ios::end);
    const std::streamsize sz = in.tellg();
    if (sz <= 0)
        return fail();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    in.read(reinterpret_cast<char*>(bytes.data()), sz);
    if (!in.good() && !in.eof())
        return fail();

    // stb_image has no TIFF support, so TIFF goes to the built-in reader.
    if (bytes.size() >= 4 &&
        ((bytes[0] == 'I' && bytes[1] == 'I') ||
         (bytes[0] == 'M' && bytes[1] == 'M')))
        return loadTiff(bytes);

    return decodeStbFromMemory(bytes.data(), bytes.size());
}

ImageBufferPtr ImageIO::loadImage(const uint8_t* data, size_t size) {
    return decodeStbFromMemory(data, size);
}

std::vector<uint8_t> ImageIO::encodeJpeg(const ImageBuffer& rgbU8,
                                         int quality) {
    std::vector<uint8_t> out;
    if (rgbU8.format() != PixelFormat::U8 || rgbU8.empty())
        return out;
    const int ok = stbi_write_jpg_to_func(
        appendToVector, &out, rgbU8.width(), rgbU8.height(), 3,
        rgbU8.data().data(), std::clamp(quality, 0, 100));
    if (ok == 0)
        out.clear();
    return out;
}

} // namespace lrc
