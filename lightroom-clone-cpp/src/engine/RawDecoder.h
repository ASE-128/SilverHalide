#pragma once

// Unified image decoder for the non-destructive pipeline. RAW camera files
// (ARW/SR2/DNG/...) are decoded by libraw; ordinary display-ready files
// (JPEG/PNG/TIFF) are decoded by stb_image and converted into the same linear
// ProPhoto working space, so every format can be imported, edited and exported
// identically. Each decode creates its own LibRaw instance; instances are
// never shared between threads.
//
// The decoder produces *linear* ProPhoto RGB (D50) at 16-bit precision for RAW
// files (libraw's output_color=4 linear ProPhoto, re-anchored to the D50
// working space by the color manager) and 32-bit float precision for
// JPEG/PNG/TIFF (converted from sRGB by the color manager). This keeps the
// non-destructive pipeline in a wide working space.

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
};

class RawDecoder {
public:
    RawDecoder() = default;
    ~RawDecoder() = default;

    // Supplies the color manager. The GUI calls this once at startup: RAW
    // decodes use it to re-anchor libraw's ProPhoto into the D50 working space,
    // and JPEG/PNG/TIFF decodes use it to convert sRGB into linear ProPhoto.
    void setColorManager(std::shared_ptr<const ColorManager> cm);

    // Returns a non-empty linear ProPhoto RGB buffer on success. Dispatches
    // on the file extension: RAW/DNG go through libraw, everything else
    // (JPEG/PNG/TIFF) through stb_image + the color manager.
    std::optional<DecodedImage> decode(const std::filesystem::path& path,
                                       const DecodeParams& params) const;

    // Extracts the embedded JPEG preview as raw bytes, if present and large
    // enough. Used by the thumbnail cache for fast browsing (RAW files only).
    std::optional<std::vector<uint8_t>> extractEmbeddedThumbnail(
        const std::filesystem::path& path) const;

    // True when `path` is a format the app can import (RAW or JPEG/PNG/TIFF).
    static bool isSupported(const std::filesystem::path& path);

private:
    // Decodes a display-ready sRGB file (JPEG/PNG/TIFF) into linear ProPhoto.
    std::optional<DecodedImage> decodeNonRaw(const std::filesystem::path& path,
                                             const DecodeParams& params) const;

    std::shared_ptr<const ColorManager> cm_;
};

} // namespace lrc
