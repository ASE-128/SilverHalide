#pragma once

// Color management wrapper around Little-CMS2. The working space is linear
// ProPhoto RGB (D50). Display and export targets are sRGB by default, or a
// user-supplied display ICC profile.
//
// All conversions run through lcms in float precision, so mid-tones do not
// band after repeated edits.

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <vector>

// Only include the lcms headers in the implementation TU.
struct _cmsHPROFILE;
struct _cmsHTRANSFORM;

namespace lrc {

class ImageBuffer;
using ImageBufferPtr = std::shared_ptr<ImageBuffer>;

class ColorManager {
public:
    ColorManager();
    ~ColorManager();

    ColorManager(const ColorManager&) = delete;
    ColorManager& operator=(const ColorManager&) = delete;

    // Linear ProPhoto input (F32 or U16) -> display-ready U8 sRGB buffer.
    ImageBufferPtr toDisplaySRGB(const ImageBuffer& linearProPhoto) const;

    // Linear ProPhoto RGB (RGB interleaved floats) -> sRGB-encoded floats
    // (gamma applied, clamped to [0,1]).
    void linearProPhotoToSRGB(const float* src, float* dst,
                              size_t pixelCount) const;

    // Converts a display-ready 8-bit sRGB buffer back into a linear ProPhoto
    // F32 buffer. Used to import JPEG/PNG/TIFF into the working space so the
    // non-destructive pipeline can edit them like raw files.
    ImageBufferPtr srgbToLinearProPhoto(const ImageBuffer& srgbU8) const;

    // Converts a source (non-RAW) image into the linear ProPhoto F32 working
    // space, honouring the source bit depth and its embedded ICC profile.
    //
    // `src` must be U8 or U16 (native depth; 16-bit sources are not truncated
    // before the transform, see R-09). `iccBytes` is the byte-serialized source
    // colour profile embedded in the file (JPEG APP2 / PNG iCCP / TIFF 34675);
    // when empty the source is assumed to be sRGB (the documented fallback, so
    // ICC-less files keep the historical behaviour). The transform runs through
    // lcms so a wide-gamut source is interpreted in its true space, not sRGB
    // (R-08).
    ImageBufferPtr sourceToLinearProPhoto(
        const ImageBuffer& src, const std::vector<uint8_t>& iccBytes) const;

    // Converts a libraw output_color=4 (linear ProPhoto, D65 white point) U16
    // buffer into the working linear ProPhoto (D50) U16 buffer. libraw's
    // ProPhoto uses the same ProPhoto primaries but a D65/equal-energy white
    // handling, so the direct cam_xyz matrix path cannot feed the pipeline;
    // this lcms adaptation re-anchors the white point while preserving
    // primaries and neutrality.
    ImageBufferPtr fromDcrawProPhoto(const ImageBuffer& dcrawProPhotoLinear) const;

    // Single-pixel variant used by the point-colour sampler: maps the 8-bit
    // sRGB pixel under the cursor back into linear ProPhoto reference RGB.
    void srgbPixelToLinearProPhoto(uint8_t r, uint8_t g, uint8_t b,
                                   float& lr, float& lg, float& lb) const;

    // Replaces the display profile with one loaded from disk (fallback to
    // sRGB if the file cannot be opened).
    void loadDisplayProfile(const std::filesystem::path& iccPath);

    // P1-2: ICC profile presets that may be embedded in exported files. sRGB is
    // the default export colour space (matches the display-render path).
    enum class ExportColorProfile { SRGB, AdobeRGB, DisplayP3 };

    /**
     * @brief Serializes a colour profile preset to ICC profile bytes.
     *
     * The bytes are suitable for embedding into an exported JPEG (APP2), PNG
     * (iCCP) or TIFF (tag 34675). Returns an empty vector on profile creation
     * or serialization failure.
     */
    static std::vector<uint8_t> exportProfileBytes(ExportColorProfile profile);

    /**
     * @brief Converts an sRGB-encoded buffer into the colour space of an ICC.
     *
     * Input `enc` must be display-ready sRGB-encoded pixels (U8 or U16). The
     * target is `iccBytes`, the exact profile bytes the caller will embed; the
     * transform runs through lcms (relative-colorimetric) so the exported
     * pixels and their embedded ICC tag describe the same colour space (R-03).
     * When `iccBytes` is empty or the target profile/transform cannot be built
     * the input is returned unchanged, so the caller never pairs converted
     * pixels against a missing label.
     *
     * @return A new buffer with the same format and dimensions as `enc`,
     *         encoded in the target ICC space (or input if no transform ran).
     */
    ImageBufferPtr sRGBEncodedToProfile(
        const ImageBuffer& enc, const std::vector<uint8_t>& iccBytes) const;

    // Combined 3x3 matrix mapping camera-linear RGB (libraw cam_xyz output,
    // D65-relative) into linear ProPhoto RGB. Row-major.
    static std::array<double, 9> cameraToProPhoto(
        const std::array<double, 9>& camToXyzD65);
    static std::array<double, 9> xyzD65ToProPhoto();

private:
    void rebuildTransforms();

    void* prophoto_ = nullptr; // cmsHPROFILE
    void* display_ = nullptr;  // cmsHPROFILE
    void* srgb_ = nullptr;     // cmsHPROFILE
    // libraw's output_color=4 working space: ProPhoto primaries, D65 white,
    // linear TRC (see fromDcrawProPhoto).
    void* dcrawProPhoto_ = nullptr; // cmsHPROFILE
    void* linearToDisplay_ = nullptr; // cmsHTRANSFORM (FLT -> FLT)
    void* srgbToLinear_ = nullptr;    // cmsHTRANSFORM (U8 sRGB -> FLT ProPhoto)
    void* dcrawToProPhoto_ = nullptr; // cmsHTRANSFORM (FLT -> FLT)

    // Shared mutex: transforms were built with cmsFLAGS_NOCACHE, so cmsDoTransform
    // may run concurrently on many threads (shared lock). Rebuilding a transform
    // (loadDisplayProfile) takes the exclusive lock so no caller uses a profile
    // that is being replaced.
    mutable std::shared_mutex mutex_;
};

} // namespace lrc
