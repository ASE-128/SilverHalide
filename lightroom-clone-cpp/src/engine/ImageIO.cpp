#include "engine/ImageIO.h"

#include <algorithm>
#include <array>
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
// linear DNG that libraw round-trips.
bool writeTiffRaw(const std::filesystem::path& path, int w, int h, int bps,
                  const uint8_t* pixels, bool dng = false) {
    if (w <= 0 || h <= 0 || !pixels || (bps != 8 && bps != 16))
        return false;
    std::ofstream os;
    if (!openBinary(path, os))
        return false;

    // IFD entries, ascending by tag. All values fit inline (<= 4 bytes).
    struct Entry {
        uint16_t tag, type;
        uint32_t count, value;
    };
    std::vector<Entry> ifd;
    ifd.push_back({256, 4, 1, static_cast<uint32_t>(w)});          // ImageWidth
    ifd.push_back({257, 4, 1, static_cast<uint32_t>(h)});          // ImageLength
    ifd.push_back({258, 3, 1, static_cast<uint32_t>(bps)});        // BitsPerSample
    ifd.push_back({259, 3, 1, 1});                                 // Compression = none
    ifd.push_back({262, 3, 1, 2});                                 // Photometric = RGB
    ifd.push_back({273, 4, 1, 0});                                 // StripOffsets (patched)
    ifd.push_back({277, 3, 1, 3});                                 // SamplesPerPixel
    ifd.push_back({278, 4, 1, static_cast<uint32_t>(h)});          // RowsPerStrip
    ifd.push_back({279, 4, 1, 0});                                 // StripByteCounts (patched)
    ifd.push_back({284, 3, 1, 1});                                 // PlanarConfig = chunky
    if (dng) {
        // DNGVersion 1.4.0.0 (BYTE[4], packed little-endian: 01 04 00 00).
        ifd.push_back({50706, 1, 4, 0x00000401u});
        // DNGBackwardVersion 1.1.0.0.
        ifd.push_back({50707, 1, 4, 0x00000101u});
        // WhiteLevel (LONG) = 65535 for a 16-bit linear file.
        ifd.push_back({50717, 4, 1, 65535u});
        // LinearRaw = 1 (data is already linear, do not gamma).
        ifd.push_back({50718, 1, 1, 1u});
        // CalibrationIlluminant1 = 21 (D65).
        ifd.push_back({50778, 3, 1, 21u});
    }
    // The strip entries sit at fixed offsets even when extra DNG tags are
    // appended: index 5 = tag 273 (StripOffsets), index 8 = tag 279
    // (StripByteCounts). The Photometric entry (index 4) must stay 2 (RGB).
    ifd[5].value = 0;                                              // patched below
    ifd[8].value = 0;                                              // patched below

    const uint32_t ifdOffset = 8;
    const uint32_t pixelOffset =
        ifdOffset + 2 + static_cast<uint32_t>(ifd.size()) * 12 + 4;
    const uint32_t rowBytes = static_cast<uint32_t>(w) * 3 * (bps / 8);
    ifd[5].value = pixelOffset;                                    // 273
    ifd[8].value = rowBytes * static_cast<uint32_t>(h);            // 279

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
             static_cast<std::streamsize>(rowBytes) * h);
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
                        const ImageBuffer& rgbU8, int quality) {
    if (rgbU8.format() != PixelFormat::U8 || rgbU8.empty())
        return false;
    std::ofstream os;
    if (!openBinary(path, os))
        return false;
    Sink sink{std::move(os)};
    const int ok = stbi_write_jpg_to_func(
        writeToSink, &sink, rgbU8.width(), rgbU8.height(), 3,
        rgbU8.data().data(), std::clamp(quality, 0, 100));
    sink.os.flush();
    return ok != 0 && sink.os.good();
}

bool ImageIO::writePng(const std::filesystem::path& path,
                       const ImageBuffer& rgbU8) {
    if (rgbU8.format() != PixelFormat::U8 || rgbU8.empty())
        return false;
    std::ofstream os;
    if (!openBinary(path, os))
        return false;
    Sink sink{std::move(os)};
    const int ok = stbi_write_png_to_func(
        writeToSink, &sink, rgbU8.width(), rgbU8.height(), 3,
        rgbU8.data().data(), rgbU8.width() * 3);
    sink.os.flush();
    return ok != 0 && sink.os.good();
}

bool ImageIO::writeTiff(const std::filesystem::path& path,
                        const ImageBuffer& rgb) {
    if (rgb.empty())
        return false;
    if (rgb.format() == PixelFormat::U8)
        return writeTiffRaw(path, rgb.width(), rgb.height(), 8,
                            rgb.data().data());
    if (rgb.format() == PixelFormat::U16)
        return writeTiffRaw(path, rgb.width(), rgb.height(), 16,
                            rgb.data().data());
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
