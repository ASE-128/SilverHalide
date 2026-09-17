#pragma once

/**
 * @brief Unified image decoder for the non-destructive pipeline.
 *
 * RAW camera files (ARW/SR2/DNG/...) are decoded by libraw; ordinary
 * display-ready files (JPEG/PNG/TIFF) are decoded by stb_image and converted
 * into the same linear ProPhoto working space, so every format can be imported,
 * edited and exported identically. Each decode creates its own LibRaw instance;
 * instances are never shared between threads.
 *
 * The decoder produces *linear* ProPhoto RGB (D50) at 16-bit precision for RAW
 * files (libraw's output_color=4 linear ProPhoto, re-anchored to the D50 working
 * space) and 32-bit float precision for JPEG/PNG/TIFF (converted from sRGB).
 * This keeps the pipeline in a wide working space.
 */

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/ImageBuffer.h"

namespace lrc {

class ColorManager;
struct DcpProfile;

struct DecodeParams {
    // Use libraw's fast half-resolution path (speeds up previews/thumbnails).
    bool halfSize = false;
    // Apply the camera's as-shot white balance. Disable to get neutral.
    bool applyCameraWB = true;
    // If > 0, the result is downscaled so its longest side fits maxDim.
    int maxDim = 0;
};

struct RawMeta {
    std::string make;
    std::string model;
    std::string lensMaker;
    std::string lensModel;
    float focalLengthMm = 0.0f;
    float aperture = 0.0f;
    float cropFactor = 1.0f;
    int width = 0;
    int height = 0;
    // Native pixel dimensions before any maxDim downscale. Useful for the 1:1
    // viewport to map a downscaled preview at true full size.
    int nativeWidth = 0;
    int nativeHeight = 0;
    // As-shot white balance multipliers relative to green.
    double asShotWBR = 1.0;
    double asShotWBG = 1.0;
    double asShotWBB = 1.0;
};

struct DecodedImage {
    // Linear ProPhoto RGB: U16 for RAW files, F32 for JPEG/PNG/TIFF.
    ImageBufferPtr linear;
    RawMeta meta;
    // Stable (non-cryptographic) hash of the embedded source ICC used for the
    // non-RAW conversion; 0 when the source had no ICC (sRGB default) or for
    // RAW files. Lets a standard-preview cache record which source colour space
    // produced its pixels, and invalidate a cached preview if the source colour
    // disposition changes.
    uint32_t sourceColorHash = 0;
};

class RawDecoder {
public:
    RawDecoder() = default;
    ~RawDecoder() = default;

    /** @brief Supplies the color manager. The GUI calls this once at startup:
     *  RAW decodes use it to re-anchor libraw's ProPhoto into the D50 working
     *  space, and JPEG/PNG/TIFF decodes use it to convert sRGB into linear
     *  ProPhoto. The manager outlives this decoder and is never mutated here. */
    void setColorManager(std::shared_ptr<const ColorManager> cm);

    /** @brief Decodes a file into a non-empty linear ProPhoto RGB buffer.
     *  Dispatches on the file extension: RAW/DNG go through libraw, everything
     *  else (JPEG/PNG/TIFF) through stb_image + the color manager.
     *  @param dcp Optional camera colour profile. When non-null and the path is
     *             a RAW file, its ColorMatrix1 replaces libraw's built-in camera
     *             matrix so a user-selected DCP actually changes the rendered
     *             colour, and its tone curve is applied afterwards. Ignored for
     *             non-RAW files. Ownership stays with the caller for the call.
     *  @return nullopt on decode failure. */
    std::optional<DecodedImage> decode(const std::filesystem::path& path,
                                       const DecodeParams& params,
                                       const DcpProfile* dcp = nullptr) const;

    /** @brief Lightweight metadata-only read for RAW paths: opens the file with
     *  libraw and returns the camera / lens / exposure metadata WITHOUT
     *  demosaicing or colour conversion (no unpack/process). Lets UI paths that
     *  serve a cached standard preview still refresh DCP/Lensfun auto-matching.
     *  @return nullopt for non-RAW paths or when the file cannot be opened. */
    std::optional<RawMeta> readMeta(const std::filesystem::path& path) const;

    /** @brief Extracts the embedded JPEG preview as raw bytes, if present and
     *  large enough. Used by the thumbnail cache for fast browsing (RAW only).
     *  @return nullopt when no usable embedded thumbnail exists. */
    std::optional<std::vector<uint8_t>> extractEmbeddedThumbnail(
        const std::filesystem::path& path) const;

    /** @brief True when `path` is a format the app can import (RAW or
     *  JPEG/PNG/TIFF). */
    static bool isSupported(const std::filesystem::path& path);

private:
    // Decodes a display-ready sRGB file (JPEG/PNG/TIFF) into linear ProPhoto.
    std::optional<DecodedImage> decodeNonRaw(const std::filesystem::path& path,
                                             const DecodeParams& params) const;

    std::shared_ptr<const ColorManager> cm_;
};

} // namespace lrc
