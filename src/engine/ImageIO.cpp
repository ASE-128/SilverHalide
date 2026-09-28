#include "engine/ImageIO.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>

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
// ICC profile embedding helpers (JPEG APP2 / PNG iCCP / TIFF tag 34675)
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

// Parses a JPEG byte stream into standalone segments (marker code + payload), in
// file order, up to — but excluding — the entropy-coded pixel data.
// Each entry holds the marker's second byte (0xE0 = APP0/JFIF, 0xDB = DQT,
// 0xEE = APP14…) and the segment body without its 2-byte length field, so a
// re-emitter must write back `payload.size() + 2` as the length.
struct JpegSegment {
    uint8_t marker = 0;              // lower byte, e.g. 0xE0 (APP0), 0xEE (APP14)
    std::vector<uint8_t> payload;    // segment body (excluding the length field)
};

/**
 * @brief Splits a JPEG into its header segments and locates the verbatim tail.
 *
 * @param jpg         Complete JPEG byte stream (must start with SOI).
 * @param pixelStart  Out: offset of the marker that begins the tail preserved
 *                    verbatim (the FFD9/FFDA marker), or jpg.size() when no such
 *                    marker was found before the stream ran out.
 * @return The parsed segments; empty when `jpg` is not a JPEG at all.
 *
 * @warning `pixelStart` points AT the 0xFF byte of the marker, not past it. A
 *          caller that reassembles the file by appending the tail from this
 *          offset keeps the scan/terminator marker; pointing past it silently
 *          produces a JPEG with no SOS, which strict decoders reject and which
 *          permissive ones decode into uninitialised output.
 *
 * @note Read-only and allocation-local, hence thread-safe.
 */
std::vector<JpegSegment> parseJpegSegments(const std::vector<uint8_t>& jpg,
                                           size_t& pixelStart) {
    std::vector<JpegSegment> segs;
    pixelStart = jpg.size();  // sentinel: nothing left to copy verbatim
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
        const size_t markerStart = i;  // offset of the 0xFF of this marker
        i += 2;
        if (marker == 0xDA) {  // Start Of Scan — header ends here
            pixelStart = markerStart;
            break;
        }
        if (marker == 0xD9) {  // EOI with no pixel data
            pixelStart = markerStart;
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

// Splits an ICC profile into ICC_PROFILE APP2 JPEG markers, per the JFIF/ICC
// convention: FFE2, length, "ICC_PROFILE\0", seq, count, data.
//
// A JPEG segment's 16-bit length field caps its payload at 65533 bytes; the
// 14-byte ICC header (12-byte signature + seq + count) leaves 65519 bytes of
// profile per marker. Splitting at that maximum keeps every profile below 64 kB
// in a single marker, which real readers need: Qt's JPEG reader silently skips
// any ICC_PROFILE chunk with a payload of 146 bytes or fewer, so a short
// trailing chunk silently truncates the profile and the colour-space parser then
// rejects it (QColorSpace::fromIccProfile: "failed size sanity"). Profiles larger
// than one marker are split into *balanced* chunks for the same reason, so that
// no chunk lands near that threshold.
//
// @return The marker bytes, or empty when the profile cannot be represented —
//         more than 255 chunks, the maximum the 1-byte count field can express.
//         The caller then embeds no profile rather than a corrupt one.
//
// @note Reassembly is caller-side; each chunk carries its own 1-based sequence
//       number and the total chunk count.
std::vector<uint8_t> jpegIccMarkers(const std::vector<uint8_t>& icc) {
    std::vector<uint8_t> out;
    constexpr size_t kMaxData = 65519;  // 65533-byte segment payload - 14
    constexpr size_t kMaxChunks = 255;  // seq/count are single bytes
    if (icc.empty())
        return out;
    const size_t count = (icc.size() + kMaxData - 1) / kMaxData;
    if (count > kMaxChunks)
        return out;
    const size_t chunk = (icc.size() + count - 1) / count;  // balanced split
    for (size_t c = 0; c < count; ++c) {
        const size_t begin = c * chunk;
        const size_t end = std::min(begin + chunk, icc.size());
        if (end <= begin)
            break;
        // JPEG segment length must account for the 2 length bytes themselves
        // (the parser subtracts them back via segLen-2). ICC header is 14 bytes:
        // "ICC_PROFILE\0"(12) + seq(1) + count(1). Off-by-two here silently
        // corrupts the ICC payload and breaks extraction.
        const size_t segLen = end - begin + 14 + 2;
        out.push_back(0xFF);
        out.push_back(0xE2);  // APP2
        out.push_back(static_cast<uint8_t>((segLen >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(segLen & 0xFF));
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
// The tail starting at the SOS marker is copied verbatim, marker included — that
// marker is what makes the result a decodable JPEG (see parseJpegSegments).
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
    // Append the rest of the header + the entropy-coded data verbatim, starting
    // at (and including) the SOS marker.
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

// ---------------------------------------------------------------------------
// Decoder input limits
// ---------------------------------------------------------------------------
// ImageBuffer's constructor *throws* on a negative dimension or an impossible
// allocation (std::invalid_argument / std::length_error / std::bad_alloc); it
// never returns an empty buffer. Each decoder must therefore reject a
// nonsensical header itself: these entry points run inside Qt slots, and an
// exception escaping a slot has no handler on the event loop.

// Upper bound on the pixel count of one decoded image (256 MP, well above any
// current consumer camera and the 50 MP performance anchors).
constexpr uint64_t kMaxDecodedPixels = 256ull * 1024ull * 1024ull;
// Upper bound on the RGB output of one decode, in bytes. The working formats
// use 1 (U8), 2 (U16) or 4 (F32) bytes per channel.
constexpr uint64_t kMaxDecodedBytes = 3ull * 1024ull * 1024ull * 1024ull;
// Upper bound on a source file that is read fully into memory before decoding.
constexpr uint64_t kMaxSourceFileBytes = 2ull * 1024ull * 1024ull * 1024ull;

/**
 * @brief True when a `w` x `h` RGB image with `bytesPerSample` bytes per
 *        channel is representable and inside the decoder limits.
 *
 * @note `w * h` cannot overflow: both operands are bounded by INT_MAX below.
 */
bool withinDecodeLimits(uint64_t w, uint64_t h, uint64_t bytesPerSample) {
    if (w == 0 || h == 0)
        return false;
    // ImageBuffer takes int dimensions, so reject anything above INT_MAX here
    // instead of letting the narrowing cast produce a negative width.
    if (w > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
        h > static_cast<uint64_t>(std::numeric_limits<int>::max()))
        return false;
    const uint64_t pixels = w * h;
    if (pixels > kMaxDecodedPixels)
        return false;
    return pixels * 3ull * bytesPerSample <= kMaxDecodedBytes;
}

// Shared stb_image decode from raw bytes into a display-ready 8-bit sRGB U8
// buffer. Returns an empty buffer on failure: malformed input and allocation
// failure are both reported through that empty buffer, never as an exception.
ImageBufferPtr decodeStbFromMemory(const uint8_t* data, size_t size) {
    auto fail = [] { return ImageBuffer::create(0, 0, PixelFormat::U8); };
    if (!data || size == 0 ||
        size > static_cast<size_t>(std::numeric_limits<int>::max()))
        return fail();
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &n, 3);
    if (!px || w <= 0 || h <= 0)
        return fail();
    if (!withinDecodeLimits(static_cast<uint64_t>(w), static_cast<uint64_t>(h), 1)) {
        stbi_image_free(px);
        return fail();
    }
    ImageBufferPtr out;
    try {
        out = ImageBuffer::create(w, h, PixelFormat::U8);
        std::memcpy(out->data().data(), px, static_cast<size_t>(w) * h * 3);
    } catch (const std::exception&) {
        stbi_image_free(px);
        return fail();
    }
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
    // T-2: adaptive rows-per-strip. A single whole-image strip (< RowsPerStrip =
    // h) keeps one contiguous w*h*3 allocation alive in readers and spikes the
    // decode memory peak on very large photos. Cap the strip at max(64, h/64)
    // rows so the strip count stays near 64 even at 100MP+, while small images
    // (h <= 64) keep their previous single-strip layout byte-identical.
    const uint32_t rowsPerStrip =
        std::max<uint32_t>(64u, (static_cast<uint32_t>(h) + 63u) / 64u);
    const uint32_t stripCount =
        (static_cast<uint32_t>(h) + rowsPerStrip - 1u) / rowsPerStrip;

    std::vector<Entry> ifd;
    ifd.push_back({256, 4, 1, static_cast<uint32_t>(w), false});          // ImageWidth
    ifd.push_back({257, 4, 1, static_cast<uint32_t>(h), false});          // ImageLength
    ifd.push_back({258, 3, 1, static_cast<uint32_t>(bps), false});        // BitsPerSample
    ifd.push_back({259, 3, 1, 1, false});                                 // Compression = none
    ifd.push_back({262, 3, 1, 2, false});                                 // Photometric = RGB
    ifd.push_back({273, 4, stripCount, 0, true});                         // StripOffsets (patched)
    ifd.push_back({277, 3, 1, 3, false});                                 // SamplesPerPixel
    ifd.push_back({278, 4, 1, rowsPerStrip, false});                      // RowsPerStrip
    ifd.push_back({279, 4, stripCount, 0, true});                         // StripByteCounts (patched)
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
    // The strip entries are patched below by scanning the tags vector (tag
    // 273 / 279 / 34675), so extra DNG/ICC tags never shift their values.
    uint32_t ifdOffset = 8;
    uint32_t ifdBytes = 2 + static_cast<uint32_t>(ifd.size()) * 12 + 4;
    const uint32_t rowBytes = static_cast<uint32_t>(w) * 3 * (bps / 8);

    // Multi-strip layout: the StripOffsets/StripByteCounts arrays are kept
    // inline after the IFD (each is stripCount LONGs); single-strip images keep
    // the value inline as before. Per-strip pixel blocks then follow, and the
    // ICC profile (padded to 4-byte alignment, matching the previous writer)
    // sits at the end.
    uint32_t arraysOffset = 0, countsOffset = 0;
    uint32_t dataStart = ifdOffset + ifdBytes;
    if (stripCount > 1) {
        arraysOffset = dataStart;
        dataStart += stripCount * 4;
        countsOffset = dataStart;
        dataStart += stripCount * 4;
    }

    std::vector<uint32_t> stripOffsets(stripCount), stripCounts(stripCount);
    uint64_t cursor = dataStart;
    for (uint32_t s = 0; s < stripCount; ++s) {
        stripOffsets[s] = static_cast<uint32_t>(cursor);
        const uint32_t rows = std::min(
            rowsPerStrip, static_cast<uint32_t>(h) - s * rowsPerStrip);
        stripCounts[s] = static_cast<uint32_t>(static_cast<uint64_t>(rowBytes) * rows);
        cursor += stripCounts[s];
    }

    uint32_t iccOffset = 0;
    const uint64_t pixelLength = cursor;
    if (icc && !icc->empty())
        iccOffset = static_cast<uint32_t>((cursor + 3) & ~uint64_t(3));

    for (auto& e : ifd) {
        if (e.tag == 273)
            e.value = (stripCount > 1) ? arraysOffset : stripOffsets[0];
        else if (e.tag == 279)
            e.value = (stripCount > 1) ? countsOffset : stripCounts[0];
        else if (e.tag == 34675 && icc && !icc->empty())
            e.value = iccOffset;
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
    if (stripCount > 1) {
        for (uint32_t o : stripOffsets)
            put32(os, o);
        for (uint32_t c : stripCounts)
            put32(os, c);
    }
    // Write each strip's pixel rows contiguously (source bytes are already
    // interleaved RGB in row-major order, so the strip split is purely linear).
    size_t srcOffset = 0;
    for (uint32_t s = 0; s < stripCount; ++s) {
        os.write(reinterpret_cast<const char*>(pixels + srcOffset),
                 static_cast<std::streamsize>(stripCounts[s]));
        srcOffset += stripCounts[s];
    }
    // ICC profile bytes (with padding to keep the file 4-byte aligned). This
    // must come after the pixel data because the strip offsets point there.
    if (icc && !icc->empty()) {
        for (uint64_t k = iccOffset - pixelLength; k > 0; --k)
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
//
// When `preserveDepth` is true a 16-bit source is returned at U16 (not
// truncated to 8 bits); otherwise the result is display-ready 8-bit
// sRGB, matching the loadImage contract. When `iccOut` is non-null the embedded
// InterColorProfile (tag 34675) bytes, if present, are copied into it.
ImageBufferPtr loadTiffImpl(const std::vector<uint8_t>& bytes, bool preserveDepth,
                            std::vector<uint8_t>* iccOut) {
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
    // Reject absurd headers (e.g. a 0xFFFFFFFF width) before the narrowing int
    // cast and the allocating ImageBuffer constructor below.
    if (!withinDecodeLimits(w, h, sampleBytes))
        return ImageBuffer::create(0, 0, PixelFormat::U8);
    const size_t bytesPerRow = static_cast<size_t>(w) * spp * sampleBytes;
    // Preserve 16-bit source depth when requested; otherwise and for
    // 8-bit sources the output is display-ready U8.
    PixelFormat outFmt = PixelFormat::U8;
    if (preserveDepth && bps == 16)
        outFmt = PixelFormat::U16;
    ImageBufferPtr out;
    try {
        out = ImageBuffer::create(static_cast<int>(w), static_cast<int>(h),
                                  outFmt);
    } catch (const std::exception&) {
        return ImageBuffer::create(0, 0, PixelFormat::U8);
    }

    // Extract the embedded ICC profile (tag 34675, type BYTE): the tag parser
    // stores each value byte sequentially in the values vector.
    if (iccOut) {
        iccOut->clear();
        auto it = tags.find(34675);
        if (it != tags.end()) {
            iccOut->reserve(it->second.size());
            for (uint32_t v : it->second)
                iccOut->push_back(static_cast<uint8_t>(v & 0xFF));
        }
    }

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
            if (outFmt == PixelFormat::U16) {
                uint16_t* dst16 = reinterpret_cast<uint16_t*>(dst);
                for (uint32_t x = 0; x < w; ++x) {
                    const size_t p = static_cast<size_t>(x) * spp * sampleBytes;
                    const uint16_t rv = row16(p), gv = row16(p + 2),
                                     bv = row16(p + 4);
                    uint16_t* px = dst16 + (static_cast<size_t>(y) * w + x) * 3;
                    px[0] = rv;
                    px[1] = gv;
                    px[2] = bv;
                }
            } else {
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
    }
    return out;
}

// Reassembles an embedded ICC profile from JPEG ICC_PROFILE APP2 markers.
// Each marker carries a 1-based sequence number and total count; the bytes must
// be reassembled in sequence order. Returns empty on no/incomplete ICC.
std::vector<uint8_t> extractIccFromJpeg(const std::vector<uint8_t>& jpg) {
    size_t pixelStart = 0;
    const auto segs = parseJpegSegments(jpg, pixelStart);
    int count = 0;
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> parts;  // (seq, data)
    for (const auto& s : segs) {
        if (s.marker != 0xE2 || s.payload.size() < 14)  // APP2 with ICC header
            continue;
        const char sig[] = "ICC_PROFILE\0";
        if (std::memcmp(s.payload.data(), sig, 12) != 0)
            continue;
        const uint8_t seq = s.payload[12];
        const uint8_t expectedCount = s.payload[13];
        count = std::max(count, static_cast<int>(expectedCount));
        parts.emplace_back(seq,
                           std::vector<uint8_t>(s.payload.begin() + 14,
                                                s.payload.end()));
    }
    if (parts.empty() || count <= 0)
        return {};
    // Assemble in sequence order; a gap means the profile is incomplete.
    std::vector<uint8_t> icc;
    for (int i = 1; i <= count; ++i) {
        bool found = false;
        for (const auto& part : parts)
            if (part.first == i) {
                icc.insert(icc.end(), part.second.begin(), part.second.end());
                found = true;
                break;
            }
        if (!found)
            return {};
    }
    return icc;
}

// PNG bit depth from the IHDR chunk (byte 8 of the IHDR data). Defaults to 8.
uint8_t pngBitDepth(const std::vector<uint8_t>& png) {
    if (png.size() >= 25 && png[12] == 'I' && png[13] == 'H' &&
        png[14] == 'D' && png[15] == 'R')
        return png[24];
    return 8;
}

// Inflates the embedded ICC from a PNG iCCP chunk. The chunk payload is
// `profile-name\0` + compression-method + zlib-deflated profile bytes; the
// decompression reuses stb_image's zlib decoder (already compiled in this TU).
// Returns empty on no iCCP chunk or inflate failure.
std::vector<uint8_t> extractIccFromPng(const std::vector<uint8_t>& png) {
    if (png.size() < 8 || png[0] != 0x89)
        return {};
    size_t pos = 8;
    while (pos + 8 <= png.size()) {
        const size_t len =
            (static_cast<size_t>(png[pos]) << 24) |
            (static_cast<size_t>(png[pos + 1]) << 16) |
            (static_cast<size_t>(png[pos + 2]) << 8) | png[pos + 3];
        const uint8_t* typ = png.data() + pos + 4;
        if (typ[0] == 'i' && typ[1] == 'C' && typ[2] == 'C' && typ[3] == 'P') {
            const size_t dataOff = pos + 8;
            if (dataOff + len > png.size())
                return {};
            const uint8_t* d = png.data() + dataOff;
            size_t nameEnd = 0;
            while (nameEnd < len && d[nameEnd] != 0)
                ++nameEnd;
            if (nameEnd + 2 > len)  // null + compression method
                return {};
            const uint8_t* zl = d + nameEnd + 2;  // past null + compression byte
            const size_t zlen = len - (nameEnd + 2);
            int outLen = 0;
            char* out = stbi_zlib_decode_malloc(
                reinterpret_cast<const char*>(zl),
                static_cast<int>(zlen), &outLen);
            if (!out || outLen <= 0)
                return {};
            std::vector<uint8_t> icc(
                reinterpret_cast<uint8_t*>(out),
                reinterpret_cast<uint8_t*>(out) + outLen);
            stbi_image_free(out);
            return icc;
        }
        pos += 8 + len + 4;
    }
    return {};
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
                       const ImageBuffer& rgbU16,
                       const std::vector<uint8_t>& icc) {
    if (rgbU16.format() != PixelFormat::U16 || rgbU16.empty())
        return false;
    const std::vector<uint8_t>* iccPtr = icc.empty() ? nullptr : &icc;
    return writeTiffRaw(path, rgbU16.width(), rgbU16.height(), 16,
                        rgbU16.data().data(), /*dng=*/true, iccPtr);
}

/**
 * @brief Decodes one image file into a display-ready U8 buffer.
 *
 * The whole file is buffered before decoding, so files above
 * kMaxSourceFileBytes are rejected outright. Every failure path -- unreadable
 * file, malformed header, allocation failure -- returns an empty buffer; no
 * exception escapes, because this runs inside Qt slots.
 */
ImageBufferPtr ImageIO::loadImage(const std::filesystem::path& path) {
    auto fail = [] { return ImageBuffer::create(0, 0, PixelFormat::U8); };
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return fail();
        in.seekg(0, std::ios::end);
        const std::streamsize sz = in.tellg();
        if (sz <= 0 || static_cast<uint64_t>(sz) > kMaxSourceFileBytes)
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
            return loadTiffImpl(bytes, /*preserveDepth=*/false, nullptr);

        return decodeStbFromMemory(bytes.data(), bytes.size());
    } catch (const std::exception&) {
        return fail();
    }
}

ImageBufferPtr ImageIO::loadImage(const uint8_t* data, size_t size) {
    return decodeStbFromMemory(data, size);
}

namespace {
// Shared decode core for the source (import) path: given raw file bytes, return
// pixels at native bit depth plus the embedded source ICC. Both the
// file-path and in-memory import APIs delegate here so they stay consistent.
//
// Throws nothing: the public entry points wrap this call in a try/catch so a
// decode-time allocation failure surfaces as an empty SourceImage.
ImageIO::SourceImage decodeSourceBytesImpl(const std::vector<uint8_t>& bytes) {
    ImageIO::SourceImage result;
    auto fail = [&result] {
        result.pixels = ImageBuffer::create(0, 0, PixelFormat::U8);
        result.icc.clear();
        return result;
    };

    // TIFF: preserve native bit depth + extract tag 34675 ICC.
    if (bytes.size() >= 4 &&
        ((bytes[0] == 'I' && bytes[1] == 'I') ||
         (bytes[0] == 'M' && bytes[1] == 'M'))) {
        result.pixels = loadTiffImpl(bytes, /*preserveDepth=*/true, &result.icc);
        if (!result.pixels || result.pixels->empty())
            return fail();
        return result;
    }

    // PNG: 16-bit sources keep their depth; extract the iCCP profile.
    if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' &&
        bytes[2] == 'N' && bytes[3] == 'G') {
        result.icc = extractIccFromPng(bytes);
        if (pngBitDepth(bytes) == 16) {
            int w = 0, h = 0, n = 0;
            stbi_us* px = stbi_load_16_from_memory(
                bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n, 3);
            if (!px || w <= 0 || h <= 0)
                return fail();
            if (!withinDecodeLimits(static_cast<uint64_t>(w),
                                    static_cast<uint64_t>(h), 2)) {
                stbi_image_free(px);
                return fail();
            }
            ImageBufferPtr out;
            try {
                out = ImageBuffer::create(w, h, PixelFormat::U16);
                std::memcpy(out->data().data(), px,
                            static_cast<size_t>(w) * h * 3 * 2);
            } catch (const std::exception&) {
                stbi_image_free(px);
                return fail();
            }
            stbi_image_free(px);
            result.pixels = std::move(out);
        } else {
            result.pixels = decodeStbFromMemory(bytes.data(), bytes.size());
        }
        if (!result.pixels || result.pixels->empty())
            return fail();
        return result;
    }

    // JPEG / BMP / other: display-ready U8 pixels + best-effort APP2 ICC.
    result.pixels = decodeStbFromMemory(bytes.data(), bytes.size());
    if (!result.pixels || result.pixels->empty())
        return fail();
    result.icc = extractIccFromJpeg(bytes);
    return result;
}
} // namespace

/**
 * @brief Import-path helper: reads a file and decodes it at native depth.
 *
 * Same failure contract as loadImage(path): over-large files are rejected and
 * every error (including allocation failure) becomes an empty SourceImage
 * instead of an exception.
 */
ImageIO::SourceImage ImageIO::loadSourceImage(
    const std::filesystem::path& path) {
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return {};
        in.seekg(0, std::ios::end);
        const std::streamsize sz = in.tellg();
        if (sz <= 0 || static_cast<uint64_t>(sz) > kMaxSourceFileBytes)
            return {};
        in.seekg(0, std::ios::beg);
        std::vector<uint8_t> bytes(static_cast<size_t>(sz));
        in.read(reinterpret_cast<char*>(bytes.data()), sz);
        if (!in.good() && !in.eof())
            return {};
        return decodeSourceBytesImpl(bytes);
    } catch (const std::exception&) {
        return {};
    }
}

ImageIO::SourceImage ImageIO::loadSourceImageFromMemory(const uint8_t* data,
                                                       size_t size) {
    if (!data || size == 0)
        return {};
    try {
        std::vector<uint8_t> bytes(data, data + size);
        return decodeSourceBytesImpl(bytes);
    } catch (const std::exception&) {
        return {};
    }
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
