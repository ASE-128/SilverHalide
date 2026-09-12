#pragma once

// Dependency-light image file I/O for the engine layer. JPEG/PNG encoding uses
// the vendored stb_image_write single header; PPM is written natively and TIFF
// (8/16-bit uncompressed RGB) by a small built-in writer. All writers go
// through std::ofstream so Unicode (e.g. Chinese) paths work on Windows.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "engine/ImageBuffer.h"

namespace lrc {

class ImageIO {
    // P1-2: an optional ICC profile (bytes of a serialized .icc) may be embedded
    // in the corresponding output formats. An empty `icc` keeps the original
    // byte-identical write path (no profile). Profiles are small (<64 kB), so a
    // single stored deflate block suffices for PNG iCCP.
    //   JPEG: embedded as ICC_PROFILE APP2 markers.
    //   PNG : embedded as an iCCP chunk (zlib stored-block deflate).
    //   TIFF: embedded as tag 34675 (InterColorProfile).
public:
    // Writes a U8 RGB buffer as baseline JPEG. quality is 0..100.
    static bool writeJpeg(const std::filesystem::path& path,
                          const ImageBuffer& rgbU8, int quality,
                          const std::vector<uint8_t>& icc = {});
    // Encodes a U8 RGB buffer to baseline JPEG bytes. Empty vector on failure.
    static std::vector<uint8_t> encodeJpeg(const ImageBuffer& rgbU8,
                                           int quality);
    // Writes a U8 RGB buffer as PNG (lossless).
    static bool writePng(const std::filesystem::path& path,
                         const ImageBuffer& rgbU8,
                         const std::vector<uint8_t>& icc = {});
    // Writes an RGB buffer as uncompressed little-endian TIFF. The bit depth
    // follows the buffer: U8 -> 8-bit, U16 -> 16-bit.
    static bool writeTiff(const std::filesystem::path& path,
                          const ImageBuffer& rgb,
                          const std::vector<uint8_t>& icc = {});
    // Writes a U8 RGB buffer as binary P6 PPM.
    static bool writePPM(const std::filesystem::path& path,
                         const ImageBuffer& rgbU8);
    // Writes a 16-bit RGB buffer as a *linear* DNG (demosaiced RGB, DNG tags,
    // photometric RGB + LinearRaw). Round-trips through RawDecoder.
    // `icc` embeds an optional ICC profile (tag 34675 InterColorProfile).
    static bool writeDng(const std::filesystem::path& path,
                         const ImageBuffer& rgbU16,
                         const std::vector<uint8_t>& icc = {});

    // --- decoding ----------------------------------------------------------
    // Loads PNG / JPEG / TIFF / BMP into a display-ready 8-bit sRGB buffer
    // (any input bit depth is converted to 8-bit). PNG/JPEG/BMP go through
    // stb_image; TIFF (uncompressed RGB, 8/16-bit) through the built-in
    // reader in ImageIO.cpp, since the vendored stb_image has no TIFF support.
    // Returns an empty buffer on failure. Use
    // ColorManager::srgbToLinearProPhoto() to feed the result into the
    // non-destructive pipeline.
    static ImageBufferPtr loadImage(const std::filesystem::path& path);
    // Loads PNG/JPEG/BMP from memory into a display-ready 8-bit sRGB buffer.
    // Returns an empty buffer on failure. (in-memory decode)
    static ImageBufferPtr loadImage(const uint8_t* data, size_t size);

    // A decoded source (import) image: native-depth pixels plus the embedded
    // source ICC, so the import path converts from the true source colour space
    // and bit depth instead of assuming sRGB (R-08/R-09).
    struct SourceImage {
        ImageBufferPtr pixels;     // U8 or U16 (16-bit is not truncated)
        std::vector<uint8_t> icc;  // embedded ICC profile bytes; empty if none
    };

    // Loads JPEG/PNG/TIFF preserving native bit depth and extracts the embedded
    // ICC profile (JPEG APP2 / PNG iCCP / TIFF tag 34675). Returns a SourceImage
    // with empty `pixels` on failure. `loadImage` above remains the display-ready
    // sRGB fast path used by cached-preview loading; use this for importing
    // files into the editing pipeline. Not applicable to RAW (libraw path).
    static SourceImage loadSourceImage(const std::filesystem::path& path);
    // Same as loadSourceImage but decodes an in-memory byte buffer (R-11), so
    // memory-based import paths restore the source ICC and bit depth exactly as
    // the file-path API does (avoiding sRGB-assumption for wide-gamut bytes).
    static SourceImage loadSourceImageFromMemory(const uint8_t* data,
                                                 size_t size);
};

} // namespace lrc
