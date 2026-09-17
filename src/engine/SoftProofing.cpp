// SoftProofing implementation.
//
// lcms2 documentation references:
//     cmsCreateProofingTransform()        — main proof pipeline
//     cmsFLAGS_GAMUTCHECK | cmsSetAlarmCodes — out-of-gamut mark-up

#include "engine/SoftProofing.h"

#include <lcms2.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace lrc {

// Small helper: maps RenderingIntent (our enum) → the lcms2 constant
static cmsUInt32Number intentCms(RenderingIntent i) {
    switch (i) {
        case RenderingIntent::Perceptual:           return INTENT_PERCEPTUAL;
        case RenderingIntent::RelativeColorimetric: return INTENT_RELATIVE_COLORIMETRIC;
        case RenderingIntent::Saturation:           return INTENT_SATURATION;
        case RenderingIntent::AbsoluteColorimetric: return INTENT_ABSOLUTE_COLORIMETRIC;
    }
    return INTENT_PERCEPTUAL;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
SoftProofing::SoftProofing() {
    // One-time profile construction for the fixed working/display legs.
    prophoto_ = cmsCreate_sRGBProfile();  // placeholder only, replaced below.
    // Actually: build correct ProPhoto (D50, linear) & sRGB profiles via the
    // same APIs ColorManager uses; this keeps the proof path gamma-matched
    // to the main pipeline.
    //
    // We don't include the ColorManager header here to keep a clean dep
    // graph; lcms has well-known built-ins.  ProPhoto is D50 (pcs), primaries
    // from ISO 22028-2:2013.  We reconstruct it via cmsCreateRGBProfile.
    cmsCIExyY whitePoint = {0.3457f / 0.3585f, 1.0f, 1.0f}; // D50 (x,y=0.3457,0.3585)
    // Recalculate to correct (x=0.34567, y=0.35850) xyY (Y=1.0 convention for cms)
    whitePoint.x = 0.34567f; whitePoint.y = 0.35850f; whitePoint.Y = 1.0f;
    cmsCIExyYTRIPLE primaries = {
        {0.7347f, 0.2653f, 1.0f}, // R
        {0.1596f, 0.8404f, 1.0f}, // G
        {0.0366f, 0.0001f, 1.0f}, // B (0.0366, 0.0001)
    };
    // Linear TRC (gamma 1.0) for the working space.
    cmsToneCurve* linearTrc[3];
    linearTrc[0] = linearTrc[1] = linearTrc[2] = cmsBuildGamma(nullptr, 1.0);
    if (prophoto_) { cmsCloseProfile(reinterpret_cast<cmsHPROFILE>(prophoto_)); prophoto_ = nullptr; }
    prophoto_ = cmsCreateRGBProfile(&whitePoint, &primaries, linearTrc);
    cmsFreeToneCurve(linearTrc[0]);

    srgb_ = cmsCreate_sRGBProfile();

    // Default alarms: pure red (1.0, 0, 0 in display space → U8 255,0,0)
    cmsUInt16Number alarms[16];
    alarms[0] = 0xFFFF; alarms[1] = 0; alarms[2] = 0;
    for (int i = 3; i < 16; ++i) alarms[i] = 0;
    cmsSetAlarmCodes(alarms);
}

SoftProofing::~SoftProofing() {
    destroyAll();
    if (prophoto_) { cmsCloseProfile(reinterpret_cast<cmsHPROFILE>(prophoto_)); prophoto_ = nullptr; }
    if (srgb_)     { cmsCloseProfile(reinterpret_cast<cmsHPROFILE>(srgb_));     srgb_     = nullptr; }
}

void SoftProofing::destroyAll() {
    if (proofDisplay_) { cmsDeleteTransform(reinterpret_cast<cmsHTRANSFORM>(proofDisplay_)); proofDisplay_ = nullptr; }
    if (proofGamut_)   { cmsDeleteTransform(reinterpret_cast<cmsHTRANSFORM>(proofGamut_));   proofGamut_   = nullptr; }
    if (proofProfile_) { cmsCloseProfile(reinterpret_cast<cmsHPROFILE>(proofProfile_));     proofProfile_ = nullptr; }
}

// ---------------------------------------------------------------------------
// Profile loading + transform rebuild
// ---------------------------------------------------------------------------
bool SoftProofing::loadProofProfile(const std::filesystem::path& iccPath) {
    std::unique_lock<std::shared_mutex> lk(mutex_);
    destroyAll();
    proofPath_.clear();
    if (iccPath.empty()) return true;   // "clear profile" success
    if (!std::filesystem::exists(iccPath)) return false;

    cmsHPROFILE hp = cmsOpenProfileFromFile(iccPath.string().c_str(), "r");
    if (!hp) return false;
    proofProfile_ = hp;
    proofPath_ = iccPath;
    rebuildTransforms(iccPath);
    return true;
}

void SoftProofing::rebuildTransforms(const std::filesystem::path& /*iccPath*/) {
    // Destroy any stale versions first (safe — callers take exclusive lock).
    if (proofDisplay_) { cmsDeleteTransform(reinterpret_cast<cmsHTRANSFORM>(proofDisplay_)); proofDisplay_ = nullptr; }
    if (proofGamut_)   { cmsDeleteTransform(reinterpret_cast<cmsHTRANSFORM>(proofGamut_));   proofGamut_   = nullptr; }

    // Both transforms operate on the full pipeline; because the rendering
    // intent is user-selectable, we rebuild on the proof-to-display call
    // instead of here.  We just do a sanity build with perceptual + no
    // gamut-check to keep hasProofProfile meaningful.  The hot path uses a
    // small local cache keyed by (intent, gamutBool).
    const auto p  = reinterpret_cast<cmsHPROFILE>(prophoto_);
    const auto pf = reinterpret_cast<cmsHPROFILE>(proofProfile_);
    const auto dp = reinterpret_cast<cmsHPROFILE>(srgb_);
    if (!p || !pf || !dp) return;

    proofDisplay_ = cmsCreateProofingTransform(
        p, TYPE_RGB_FLT,        // input:  ProPhoto profile + F32 interleaved buffer
        pf, TYPE_RGB_8,         // simulated output profile + display U8 buffer
        dp,                     // preview (monitor) profile
        INTENT_PERCEPTUAL,      // document → proofing-device intent (rebuilt later)
        INTENT_RELATIVE_COLORIMETRIC, // proofing-device → monitor intent
        cmsFLAGS_NOCACHE | cmsFLAGS_BLACKPOINTCOMPENSATION);
}

bool SoftProofing::hasProofProfile() const {
    std::shared_lock<std::shared_mutex> lk(mutex_);
    return proofProfile_ != nullptr;
}

std::filesystem::path SoftProofing::proofProfilePath() const {
    std::shared_lock<std::shared_mutex> lk(mutex_);
    return proofPath_;
}

// ---------------------------------------------------------------------------
// Per-call cached proof transform (intent / gamut-dependent small cache).
// ---------------------------------------------------------------------------
namespace {
struct ProofTransformKey {
    uint32_t intent : 2;
    uint32_t gamut  : 1;
    bool operator==(ProofTransformKey o) const {
        return intent == o.intent && gamut == o.gamut;
    }
};
} // namespace

ImageBufferPtr SoftProofing::proofToDisplay(const ImageBuffer& input,
                                            RenderingIntent intent,
                                            bool withGamutCheck) const {
    std::shared_lock<std::shared_mutex> rl(mutex_);
    if (!proofProfile_ || !prophoto_ || !srgb_) return nullptr;

    const int w = input.width(), h = input.height();
    if (w <= 0 || h <= 0) return nullptr;

    // Build (or reuse) a cms transform for the specific (intent, gamut) pair.
    // Because lcms2 transforms are not cheap to create and immutable after,
    // we maintain a 4-entry small cache.  We leak the handles into process
    // lifetime (max 4) to avoid cross-thread destruction complexity.
    struct CachedXform { std::shared_mutex m; void* xform = nullptr; };
    static CachedXform cache[2][4]; // [gamut?0:1][intent]

    const int gi = withGamutCheck ? 1 : 0;
    const int ii = std::min(3, std::max(0, static_cast<int>(intent)));
    auto& slot = cache[gi][ii];
    void* xform = nullptr;
    {
        std::shared_lock<std::shared_mutex> kl(slot.m);
        xform = slot.xform;
    }
    if (!xform) {
        std::unique_lock<std::shared_mutex> kl(slot.m);
        if (!slot.xform) {
            const cmsUInt32Number baseFlags = cmsFLAGS_NOCACHE | cmsFLAGS_BLACKPOINTCOMPENSATION |
                                              (withGamutCheck ? cmsFLAGS_GAMUTCHECK : 0);
            slot.xform = cmsCreateProofingTransform(
                reinterpret_cast<cmsHPROFILE>(prophoto_), TYPE_RGB_FLT,
                reinterpret_cast<cmsHPROFILE>(proofProfile_), TYPE_RGB_8,
                reinterpret_cast<cmsHPROFILE>(srgb_),
                intentCms(intent),
                INTENT_RELATIVE_COLORIMETRIC,
                baseFlags);
        }
        xform = slot.xform;
    }
    if (!xform) return nullptr;

    // Input: ensure F32 interleaved ProPhoto.
    ImageBufferPtr f32Src;
    const float* srcFloat;
    if (input.format() == PixelFormat::F32) {
        srcFloat = reinterpret_cast<const float*>(input.data().data());
    } else {
        f32Src = input.toF32();
        srcFloat = reinterpret_cast<const float*>(f32Src->data().data());
    }

    auto out = ImageBuffer::create(w, h, PixelFormat::U8);
    const size_t pixels = static_cast<size_t>(w) * h;
    // cmsDoTransform processes the whole buffer in one go.  For F32->U8 the
    // stride is 3 floats in : 3 bytes out per pixel.
    cmsDoTransform(reinterpret_cast<cmsHTRANSFORM>(xform),
                   const_cast<float*>(srcFloat), out->data().data(),
                   static_cast<cmsUInt32Number>(pixels));

    return out;
}

ImageBufferPtr SoftProofing::gamutMask(const ImageBuffer& input,
                                       RenderingIntent intent) const {
    // Run proof-to-display with gamutCheck enabled, then convert "pixel was
    // marked as alarm code" into a F32 weight mask.  lcms sets out-of-gamut
    // pixels to the alarm code (255,0,0 in the default config).
    auto proof = proofToDisplay(input, intent, true);
    if (!proof) return nullptr;
    const int w = proof->width(), h = proof->height();
    auto mask = ImageBuffer::create(w, h, PixelFormat::F32);
    const uint8_t* s = proof->data().data();
    float* d = reinterpret_cast<float*>(mask->data().data());
    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n; ++i) {
        const size_t si = i * 3;
        const bool outOfGamut = (s[si] >= 250 && s[si+1] <= 5 && s[si+2] <= 5);
        const float v = outOfGamut ? 1.0f : 0.0f;
        d[si    ] = v;
        d[si + 1] = v;
        d[si + 2] = v;
    }
    return mask;
}

} // namespace lrc
