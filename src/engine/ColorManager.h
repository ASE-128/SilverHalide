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

    /**
     * @brief Renders a linear ProPhoto buffer to display-ready U8 sRGB.
     *
     * @param linearProPhoto Linear ProPhoto source (F32 or U16).
     * @return A new U8 sRGB buffer with the same dimensions as the input, or
     *         an empty 0x0 buffer when the input is empty. Never nullptr.
     * @note Allocates the output buffer (and an intermediate F32 copy for U16
     *       input), so it may throw std::bad_alloc.
     */
    [[nodiscard]] ImageBufferPtr toDisplaySRGB(
        const ImageBuffer& linearProPhoto) const;

    // Linear ProPhoto RGB (RGB interleaved floats) -> sRGB-encoded floats
    // (gamma applied, clamped to [0,1]).
    void linearProPhotoToSRGB(const float* src, float* dst,
                              size_t pixelCount) const;

    /**
     * @brief Converts a display-ready 8-bit sRGB buffer back to linear ProPhoto.
     *
     * Used to import JPEG/PNG/TIFF into the working space so the
     * non-destructive pipeline can edit them like raw files.
     *
     * @param srgbU8 Source buffer; must be U8.
     * @return A new F32 linear ProPhoto buffer with the same dimensions, or an
     *         empty 0x0 buffer when the input is empty or not U8.
     * @note Allocates the output buffer, so it may throw std::bad_alloc.
     */
    [[nodiscard]] ImageBufferPtr srgbToLinearProPhoto(
        const ImageBuffer& srgbU8) const;

    /**
     * @brief Converts a source (non-RAW) image into linear ProPhoto F32.
     *
     * The source bit depth and its embedded ICC profile are honoured: `src`
     * must be U8 or U16 (native depth; 16-bit sources are not truncated before
     * the transform). `iccBytes` is the byte-serialized source colour profile
     * embedded in the file (JPEG APP2 / PNG iCCP / TIFF 34675); when empty the
     * source is assumed to be sRGB (the documented fallback, so ICC-less files
     * keep the historical behaviour). The transform runs through lcms so a
     * wide-gamut source is interpreted in its true space, not sRGB.
     *
     * @param src Source pixels; U8 or U16 only.
     * @param iccBytes Embedded ICC bytes, or empty for the sRGB fallback. The
     *        span is borrowed for the duration of the call only.
     * @return A new F32 linear ProPhoto buffer with the source dimensions, or
     *         an empty 0x0 buffer when `src` is empty / not U8 or U16, or when
     *         no lcms transform could be built.
     * @note Allocates and may throw std::bad_alloc; it is safe to call
     *       concurrently from several threads (the transforms are read-only
     *       under the internal shared lock).
     */
    [[nodiscard]] ImageBufferPtr sourceToLinearProPhoto(
        const ImageBuffer& src, const std::vector<uint8_t>& iccBytes) const;

    /**
     * @brief Re-anchors libraw's D65 ProPhoto output to the D50 working space.
     *
     * libraw output_color=4 is linear ProPhoto with a D65/equal-energy white
     * handling: the same ProPhoto primaries, but a different white point, so
     * the direct cam_xyz matrix path cannot feed the pipeline. This lcms
     * adaptation re-anchors the white point while preserving primaries and
     * neutrality.
     *
     * @param dcrawProPhotoLinear U16 linear ProPhoto (D65) source buffer.
     * @return A new U16 linear ProPhoto (D50) buffer with the input
     *         dimensions, or an empty 0x0 U16 buffer when the input is empty
     *         or not U16.
     * @note Allocates and may throw std::bad_alloc; it is safe to call
     *       concurrently from several threads.
     */
    [[nodiscard]] ImageBufferPtr fromDcrawProPhoto(
        const ImageBuffer& dcrawProPhotoLinear) const;

    // Single-pixel variant used by the point-colour sampler: maps the 8-bit
    // sRGB pixel under the cursor back into linear ProPhoto reference RGB.
    void srgbPixelToLinearProPhoto(uint8_t r, uint8_t g, uint8_t b,
                                   float& lr, float& lg, float& lb) const;

    // Replaces the display profile with one loaded from disk (fallback to
    // sRGB if the file cannot be opened).
    void loadDisplayProfile(const std::filesystem::path& iccPath);

    // ICC profile presets that may be embedded in exported files. sRGB is
    // the default export colour space (matches the display-render path).
    enum class ExportColorProfile { SRGB, AdobeRGB, DisplayP3 };

    /**
     * @brief Serializes a colour profile preset to ICC profile bytes.
     *
     * The bytes are suitable for embedding into an exported JPEG (APP2), PNG
     * (iCCP) or TIFF (tag 34675). Returns an empty vector on profile creation
     * or serialization failure.
     *
     * @note The result is deterministic: the profile's creation timestamp is
     *       pinned (see the implementation), so the same preset yields
     *       byte-identical bytes in every process and on every call. Exports made
     *       with the same settings are therefore byte-reproducible.
     */
    [[nodiscard]] static std::vector<uint8_t> exportProfileBytes(
        ExportColorProfile profile);

    /**
     * @brief Converts an sRGB-encoded buffer into the colour space of an ICC.
     *
     * Input `enc` must be display-ready sRGB-encoded pixels (U8 or U16). The
     * target is `iccBytes`, the exact profile bytes the caller will embed; the
     * transform runs through lcms (relative-colorimetric) so the exported
     * pixels and their embedded ICC tag describe the same colour space.
     * When `iccBytes` is empty or the target profile/transform cannot be built
     * the input is returned unchanged, so the caller never pairs converted
     * pixels against a missing label.
     *
     * @return A new buffer with the same format and dimensions as `enc`,
     *         encoded in the target ICC space (or input if no transform ran).
     */
    [[nodiscard]] ImageBufferPtr sRGBEncodedToProfile(
        const ImageBuffer& enc, const std::vector<uint8_t>& iccBytes) const;

    /**
     * @brief Builds the camera-linear RGB -> linear ProPhoto RGB matrix.
     *
     * @param camToXyzD65 Camera-linear RGB -> XYZ D65 matrix (libraw cam_xyz
     *        output, D65-relative), row-major.
     * @return Combined 3x3 matrix mapping camera-linear RGB into linear
     *         ProPhoto RGB (D50), row-major.
     * @note Pure arithmetic on fixed-size arrays: fixed input/output, no
     *       allocation, so it is noexcept and safe from any thread.
     */
    [[nodiscard]] static std::array<double, 9> cameraToProPhoto(
        const std::array<double, 9>& camToXyzD65) noexcept;

    /**
     * @brief Returns the XYZ (D65) -> linear ProPhoto RGB (D50) matrix.
     *
     * @return Row-major 3x3 matrix; the Bradford D65->D50 adaptation composed
     *         with the XYZ(D50) -> ProPhoto conversion.
     * @note Pure arithmetic on fixed-size arrays: noexcept, no allocation.
     */
    [[nodiscard]] static std::array<double, 9> xyzD65ToProPhoto() noexcept;

private:
    // --- lcms handle ownership ---------------------------------------------
    // The handle types stay opaque (void*) so this header remains free of
    // <lcms2.h>; the closing calls live in ColorManager.cpp where lcms is
    // available. Both deleters are noexcept on purpose: lcms teardown does not
    // throw, and a throwing deleter inside a destructor would std::terminate.
    struct CmsProfileDeleter {
        void operator()(void* handle) const noexcept;
    };
    struct CmsTransformDeleter {
        void operator()(void* handle) const noexcept;
    };
    using CmsProfile = std::unique_ptr<void, CmsProfileDeleter>;
    using CmsTransform = std::unique_ptr<void, CmsTransformDeleter>;

    void rebuildTransforms();

    /**
     * @brief Returns the lcms profile the display transform targets.
     *
     * The display target is the user-loaded override (ownedDisplay_) when one
     * is loaded, otherwise the built-in sRGB profile (srgb_). Deriving the
     * target from ownership (instead of storing a second pointer that aliases
     * srgb_ in the default state) guarantees a single owner per handle: no
     * handle can be closed twice and no destructor-order contract is needed
     * between srgb_ and the display profile.
     *
     * @return Borrowed lcms profile handle: never close it. It is valid while
     *         this ColorManager is alive and no profile is being replaced, so a
     *         caller must hold mutex_ (shared or exclusive) as the conversion
     *         paths in the implementation do.
     */
    void* activeDisplayProfile() const noexcept {
        return ownedDisplay_ ? ownedDisplay_.get() : srgb_.get();
    }

    // Declaration order is the destruction-order contract: members are
    // destroyed in reverse order, so the three transforms (declared last) are
    // released before the profiles they were built from. Every unique_ptr is
    // the single close site for its handle - no manual cmsCloseProfile /
    // cmsDeleteTransform calls remain - so a constructor that fails midway
    // reclaims whatever was already built by unwinding through these members.
    CmsProfile prophoto_;      // linear ProPhoto RGB (D50) working space
    CmsProfile srgb_;          // built-in sRGB display/export space
    CmsProfile ownedDisplay_;  // loaded display profile; empty => srgb_ is used
    // libraw's output_color=4 working space: ProPhoto primaries, D65 white,
    // linear TRC (see fromDcrawProPhoto).
    CmsProfile dcrawProPhoto_;
    CmsTransform linearToDisplay_;  // cmsHTRANSFORM (FLT -> FLT)
    CmsTransform srgbToLinear_;     // cmsHTRANSFORM (U8 sRGB -> FLT ProPhoto)
    CmsTransform dcrawToProPhoto_;  // cmsHTRANSFORM (FLT -> FLT)

    // Shared mutex: transforms were built with cmsFLAGS_NOCACHE, so cmsDoTransform
    // may run concurrently on many threads (shared lock). Rebuilding a transform
    // (loadDisplayProfile) takes the exclusive lock so no caller uses a profile
    // that is being replaced.
    mutable std::shared_mutex mutex_;
};

} // namespace lrc
