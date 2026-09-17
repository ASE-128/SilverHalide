#pragma once

/**
 * @file ImageIO.h
 * @brief Dependency-light image file I/O for the engine layer.
 *
 * JPEG/PNG encoding uses the vendored stb_image_write single header; PPM is
 * written natively and TIFF (8/16-bit uncompressed RGB) by a small built-in
 * writer. All writers go through std::ofstream so Unicode (e.g. Chinese)
 * paths work on Windows.
 */

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "engine/ImageBuffer.h"

namespace lrc {

/**
 * @brief Static image encode/decode helpers for the engine layer. Thread-safe:
 *        every method is stateless apart from its arguments and may be called
 *        from any thread concurrently.
 *
 * An optional ICC profile (bytes of a serialized .icc) may be embedded in the
 * corresponding output formats. An empty `icc` keeps the original byte-identical
 * write path (no profile). Profiles are small (<64 kB), so a single stored
 * deflate block suffices for PNG iCCP.
 *   JPEG: embedded as ICC_PROFILE APP2 markers.
 *   PNG : embedded as an iCCP chunk (zlib stored-block deflate).
 *   TIFF: embedded as tag 34675 (InterColorProfile).
 */
class ImageIO {
public:
    /** @brief Writes a U8 RGB buffer as baseline JPEG.
     *  @param quality 0..100. @return false on I/O or encode failure. */
    static bool writeJpeg(const std::filesystem::path& path,
                          const ImageBuffer& rgbU8, int quality,
                          const std::vector<uint8_t>& icc = {});
    /** @brief Encodes a U8 RGB buffer to baseline JPEG bytes.
     *  @return Empty vector on failure. */
    static std::vector<uint8_t> encodeJpeg(const ImageBuffer& rgbU8,
                                           int quality);
    /** @brief Writes a U8 RGB buffer as PNG (lossless).
     *  @return false on I/O or encode failure. */
    static bool writePng(const std::filesystem::path& path,
                         const ImageBuffer& rgbU8,
                         const std::vector<uint8_t>& icc = {});
    /** @brief Writes an RGB buffer as uncompressed little-endian TIFF. The bit
     *  depth follows the buffer: U8 -> 8-bit, U16 -> 16-bit.
     *  @return false on failure or unsupported buffer format. */
    static bool writeTiff(const std::filesystem::path& path,
                          const ImageBuffer& rgb,
                          const std::vector<uint8_t>& icc = {});
    /** @brief Writes a U8 RGB buffer as binary P6 PPM.
     *  @return false on I/O failure. */
    static bool writePPM(const std::filesystem::path& path,
                         const ImageBuffer& rgbU8);
    /** @brief Writes a 16-bit RGB buffer as a *linear* DNG (demosaiced RGB, DNG
     *  tags, photometric RGB + LinearRaw). Round-trips through RawDecoder.
     *  @param icc Optional ICC profile (tag 34675 InterColorProfile).
     *  @return false on failure. */
    static bool writeDng(const std::filesystem::path& path,
                         const ImageBuffer& rgbU16,
                         const std::vector<uint8_t>& icc = {});

    /** @brief Loads PNG / JPEG / TIFF / BMP into a display-ready 8-bit sRGB
     *  buffer (any input bit depth is converted to 8-bit). PNG/JPEG/BMP go
     *  through stb_image; TIFF (uncompressed RGB, 8/16-bit) through the built-in
     *  reader since stb_image has no TIFF support.
     *  @return An empty buffer on failure. Feed the result into
     *          ColorManager::srgbToLinearProPhoto() for the pipeline. */
    static ImageBufferPtr loadImage(const std::filesystem::path& path);
    /** @brief Loads PNG/JPEG/BMP from memory into a display-ready 8-bit sRGB
     *  buffer (in-memory decode).
     *  @return Empty buffer on failure. */
    static ImageBufferPtr loadImage(const uint8_t* data, size_t size);

    /** @brief A decoded source (import) image: native-depth pixels plus the
     *  embedded source ICC, so the import path converts from the true source
     *  colour space and bit depth instead of assuming sRGB. */
    struct SourceImage {
        ImageBufferPtr pixels;     // U8 or U16 (16-bit is not truncated)
        std::vector<uint8_t> icc;  // embedded ICC profile bytes; empty if none
    };

    /** @brief Loads JPEG/PNG/TIFF preserving native bit depth and extracts the
     *  embedded ICC profile (JPEG APP2 / PNG iCCP / TIFF tag 34675).
     *  @return A SourceImage with empty `pixels` on failure. This is the import
     *          path into the editing pipeline; loadImage() is the display-ready
     *          sRGB fast path for cached-preview loading. Not for RAW (libraw
     *          path). */
    static SourceImage loadSourceImage(const std::filesystem::path& path);
    /** @brief Same as loadSourceImage but decodes an in-memory byte buffer, so
     *  memory-based import paths restore the source ICC and bit depth exactly
     *  as the file-path API does (avoiding an sRGB assumption for wide-gamut
     *  bytes). */
    static SourceImage loadSourceImageFromMemory(const uint8_t* data,
                                                 size_t size);
};

} // namespace lrc
