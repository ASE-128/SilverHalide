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
public:
    // Writes a U8 RGB buffer as baseline JPEG. quality is 0..100.
    static bool writeJpeg(const std::filesystem::path& path,
                          const ImageBuffer& rgbU8, int quality);
    // Encodes a U8 RGB buffer to baseline JPEG bytes. Empty vector on failure.
    static std::vector<uint8_t> encodeJpeg(const ImageBuffer& rgbU8,
                                           int quality);
    // Writes a U8 RGB buffer as PNG (lossless).
    static bool writePng(const std::filesystem::path& path,
                         const ImageBuffer& rgbU8);
    // Writes an RGB buffer as uncompressed little-endian TIFF. The bit depth
    // follows the buffer: U8 -> 8-bit, U16 -> 16-bit.
    static bool writeTiff(const std::filesystem::path& path,
                          const ImageBuffer& rgb);
    // Writes a U8 RGB buffer as binary P6 PPM.
    static bool writePPM(const std::filesystem::path& path,
                         const ImageBuffer& rgbU8);
    // Writes a 16-bit RGB buffer as a *linear* DNG (demosaiced RGB, DNG tags,
    // photometric RGB + LinearRaw). Round-trips through RawDecoder.
    static bool writeDng(const std::filesystem::path& path,
                         const ImageBuffer& rgbU16);

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
};

} // namespace lrc
