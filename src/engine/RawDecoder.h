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
     *             a RAW file, its (white-balance-interpolated) colour matrix
     *             replaces libraw's built-in camera matrix, and its hue/sat
     *             map and tone curve are applied afterwards in the linear
     *             working space, so a user-selected DCP actually changes the
     *             rendered colour. Ignored for non-RAW files. Ownership stays
     *             with the caller for the call.
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

    /**
     * @brief White-balance-aware camera->XYZ D50 matrix from a DCP profile.
     *
     * When the profile carries both calibration states (ColorMatrix1/2 plus
     * CalibrationIlluminant1/2), the matrices are interpolated by the
     * correlated color temperature of the as-shot white balance, following
     * the DNG dual-illuminant model. With a single state the matrix is used
     * verbatim.
     *
     * @param dcp Parsed DCP profile; `hasColorMatrix1` must be true.
     * @param camMul As-shot white balance multipliers (camera RGB gains,
     *        relative scaling is sufficient).
     * @return The interpolated 3x3 row-major camera->XYZ D50 matrix.
     */
    static std::array<float, 9> dcpColorMatrix(
        const DcpProfile& dcp, const std::array<double, 3>& camMul);

    /**
     * @brief Applies a DCP hue/saturation/value map to a linear buffer.
     *
     * Implements the DNG hue/sat map in the linear working space: per pixel
     * the RGB triplet is converted to HSV, the (hue shift, saturation scale,
     * value scale) delta is sampled with trilinear interpolation (hue wraps,
     * saturation/value clamp), applied, and converted back to RGB. When the
     * profile carries a second map it is blended by the same dual-illuminant
     * weight as the colour matrix.
     *
     * @param src Linear RGB buffer (F32, normalized 0..1).
     * @param dcp Parsed DCP profile with a valid hue/sat map.
     * @param camMul As-shot white balance multipliers (see dcpColorMatrix).
     * @return New graded buffer, or `src` unchanged when the profile has no
     *         usable map or the input is not F32.
     */
    static ImageBufferPtr applyDcpHueSatMap(
        const ImageBufferPtr& src, const DcpProfile& dcp,
        const std::array<double, 3>& camMul);

private:
    // Decodes a display-ready sRGB file (JPEG/PNG/TIFF) into linear ProPhoto.
    std::optional<DecodedImage> decodeNonRaw(const std::filesystem::path& path,
                                             const DecodeParams& params) const;

    std::shared_ptr<const ColorManager> cm_;
};

} // namespace lrc
