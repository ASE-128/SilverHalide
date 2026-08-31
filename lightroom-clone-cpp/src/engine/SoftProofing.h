#pragma once

// ---------------------------------------------------------------------------
// PHASE_15: SoftProofing — soft-proof wrapper over Little-CMS2.
//
// Creates (and caches) an lcms2 proofing transform:
//
//     Working space (ProPhoto, linear) → Simulated printer profile → Display (sRGB)
//
// plus an optional Gamut-check transform that marks every out-of-gamut pixel
// with an alarm code (1,0,0 pure red = Lightroom-style overlay colour).
//
// Pure C++.  Engine/ — no Qt.  The caller (UI / viewport) is responsible for
// overlaying the gamut alarm on top of the preview and for loading the
// proof profile from disk; this class only deals with lcms2 buffers.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "engine/ImageBuffer.h"

namespace lrc {

/** Rendering intent for the proofing transform (matches lcms2 constants). */
enum class RenderingIntent : uint8_t {
    Perceptual = 0,             // cmsIntentPerceptual (0)
    RelativeColorimetric = 1,   // cmsIntentRelativeColorimetric (1)
    Saturation = 2,             // cmsIntentSaturation (2)
    AbsoluteColorimetric = 3,   // cmsIntentAbsoluteColorimetric (3)
};

/** Alarm codes used to tint out-of-gamut pixels in the gamut-check transform. */
struct GamutAlarmCodes {
    uint8_t r = 255;  // default = pure red (Lightroom's gamut warning colour)
    uint8_t g = 0;
    uint8_t b = 0;
};

/**
 * @brief Bundle of soft-proof parameters carried between the UI dialog and
 *        the rendering pipeline (ImageViewport / MainWindow).
 *
 * The engine-level SoftProofing class takes these fields *exploded* as
 * arguments (see proofToDisplay / gamutMask); this struct is the UI-layer
 * carrier so slots and signals can ship a single value.  Keeping it here
 * (engine/) lets both the Qt-free engine tests and the Qt UI layer depend
 * on the same layout without pulling QtWidgets into the engine.
 */
struct SoftProofParams {
    /** Filesystem path of the destination ICC profile, or a ColorManager
     *  built-in key ("srgb", "display_p3", "adobe_rgb", "prophoto",
     *  "swop_coated_gracol", "fogra39") that the caller resolves. */
    std::string destinationProfilePath;

    RenderingIntent intent = RenderingIntent::Perceptual;

    /** Map the source black to the destination black; avoids crushed
     *  shadows on printing proofs.  Maps to cmsFLAGS_BLACKPOINTCOMPENSATION. */
    bool useBlackPointCompensation = true;

    /** When true + intent == AbsoluteColorimetric, the proof transform
     *  simulates the paper white point (subtle yellowing for newsprint etc.). */
    bool simulatePaperWhite = false;

    /** Toggle the gamut-warning overlay (out-of-gamut pixels get tinted). */
    bool showOutOfGamut = false;

    /** Gamut-warning overlay colour in normalised 0..1 sRGB floats. */
    float outOfGamutR = 1.0f;
    float outOfGamutG = 0.0f;
    float outOfGamutB = 0.0f;
};

class SoftProofing {
public:
    SoftProofing();
    ~SoftProofing();

    SoftProofing(const SoftProofing&) = delete;
    SoftProofing& operator=(const SoftProofing&) = delete;

    /** (Re)load the proof profile from an .icc / .icm path.  Returns false
     *  when the file is unreadable or not a valid profile.  Passing an
     *  empty path clears any loaded profile. */
    bool loadProofProfile(const std::filesystem::path& iccPath);

    /** True iff a valid proof profile has been loaded. */
    bool hasProofProfile() const;

    /**
     * @brief Apply the proofing transform to a working-space buffer.
     *
     * @param linearProPhoto   Input F32 ProPhoto RGB interleaved (never mutated).
     * @param intent           Which rendering intent to use for the proof leg.
     * @param withGamutCheck   When true, every out-of-gamut pixel gets its
     *                         R ch set to 1.0 (the Lightroom-style overlay).
     * @return                 Display-ready U8 sRGB buffer — same dimensions.
     */
    ImageBufferPtr proofToDisplay(const ImageBuffer& linearProPhoto,
                                  RenderingIntent intent,
                                  bool withGamutCheck) const;

    /**
     * @brief Return a per-pixel U8 boolean mask (packed as RRR F32 grey) where
     *        1.0 = out-of-gamut under the current proof profile.  Returns null
     *        when no proof profile has been loaded.  Used by the UI to build
     *        a red overlay at any desired transparency (Shift+G toggle).
     */
    ImageBufferPtr gamutMask(const ImageBuffer& linearProPhoto,
                             RenderingIntent intent) const;

    /** Path of the currently loaded profile (empty when none). */
    std::filesystem::path proofProfilePath() const;

private:
    void rebuildTransforms(const std::filesystem::path& fallbackPath);
    void destroyAll();

    // Opaque cmsHPROFILE / cmsHTRANSFORM handles.
    void* proofProfile_ = nullptr;   // cmsHPROFILE (destination simulated device)
    void* prophoto_     = nullptr;   // cmsHPROFILE (cached working space)
    void* srgb_         = nullptr;   // cmsHPROFILE (display space for the proof leg)
    void* proofDisplay_ = nullptr;   // cmsHTRANSFORM: ProPhoto → [proof] → sRGB
    void* proofGamut_   = nullptr;   // cmsHTRANSFORM with FLAGS_GAMUTCHECK
    std::filesystem::path proofPath_;
    mutable std::shared_mutex mutex_;
};

} // namespace lrc
