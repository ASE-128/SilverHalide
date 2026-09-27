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
    // Private lcms context for the whole instance. Everything below (profiles,
    // transforms, and the gamut alarm codes) is created on this context, which
    // is what keeps proofing state from leaking into unrelated transforms: the
    // alarm codes live in an lcms "client chunk" that a transform reads on every
    // run, so using the process-global context would recolour any concurrent
    // transform and race with it.
    ctx_ = cmsCreateContext(nullptr, nullptr);

    // One-time profile construction for the fixed working/display legs.
    prophoto_ = cmsCreate_sRGBProfileTHR(reinterpret_cast<cmsContext>(ctx_));
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
    prophoto_ = cmsCreateRGBProfileTHR(reinterpret_cast<cmsContext>(ctx_),
                                       &whitePoint, &primaries, linearTrc);
    cmsFreeToneCurve(linearTrc[0]);

    srgb_ = cmsCreate_sRGBProfileTHR(reinterpret_cast<cmsContext>(ctx_));

    // Default alarm: pure red (1.0, 0, 0 in display space → U8 255,0,0).
    setGamutAlarmCodes(alarm_.r, alarm_.g, alarm_.b);
}

SoftProofing::~SoftProofing() {
    destroyAll();
    if (prophoto_) { cmsCloseProfile(reinterpret_cast<cmsHPROFILE>(prophoto_)); prophoto_ = nullptr; }
    if (srgb_)     { cmsCloseProfile(reinterpret_cast<cmsHPROFILE>(srgb_));     srgb_     = nullptr; }
    // Must outlive every profile/transform created on it.
    if (ctx_)      { cmsDeleteContext(reinterpret_cast<cmsContext>(ctx_));      ctx_      = nullptr; }
}

void SoftProofing::setGamutAlarmCodes(uint8_t r, uint8_t g, uint8_t b) {
    std::unique_lock<std::shared_mutex> lk(mutex_);
    alarm_ = GamutAlarmCodes{r, g, b};
    if (!ctx_)
        return;
    // lcms reads the codes from the context on every cmsDoTransform, so no
    // transform has to be rebuilt; the exclusive lock is what keeps this from
    // interleaving with a transform that is currently running.
    cmsUInt16Number codes[cmsMAXCHANNELS] = {};
    codes[0] = static_cast<cmsUInt16Number>(static_cast<uint32_t>(r) * 257u);
    codes[1] = static_cast<cmsUInt16Number>(static_cast<uint32_t>(g) * 257u);
    codes[2] = static_cast<cmsUInt16Number>(static_cast<uint32_t>(b) * 257u);
    cmsSetAlarmCodesTHR(reinterpret_cast<cmsContext>(ctx_), codes);
}

GamutAlarmCodes SoftProofing::gamutAlarmCodes() const {
    std::shared_lock<std::shared_mutex> lk(mutex_);
    return alarm_;
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

    cmsHPROFILE hp = cmsOpenProfileFromFileTHR(
        reinterpret_cast<cmsContext>(ctx_), iccPath.string().c_str(), "r");
    if (!hp) return false;
    proofProfile_ = hp;
    proofPath_ = iccPath;
    // Bump the generation so the per-slot transform cache (proofCache_)
    // rebuilds against this new profile instead of reusing the previous one.
    ++cacheGeneration_;
    rebuildTransforms(iccPath);
    return true;
}

bool SoftProofing::loadProofProfileFromMemory(
    const std::vector<uint8_t>& iccBytes) {
    std::unique_lock<std::shared_mutex> lk(mutex_);
    destroyAll();
    proofPath_.clear();
    if (iccBytes.empty()) return true;   // "clear profile" success
    cmsHPROFILE hp = cmsOpenProfileFromMemTHR(
        reinterpret_cast<cmsContext>(ctx_), iccBytes.data(),
        static_cast<cmsUInt32Number>(iccBytes.size()));
    if (!hp) return false;
    proofProfile_ = hp;
    // No path: the transform cache is still invalidated so a subsequent
    // proofToDisplay() rebuilds against the in-memory profile.
    ++cacheGeneration_;
    rebuildTransforms(std::filesystem::path{});
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

    proofDisplay_ = cmsCreateProofingTransformTHR(
        reinterpret_cast<cmsContext>(ctx_),
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
    // we keep a small per-instance cache indexed by (intent, gamut).  Each
    // slot is stamped with the cache generation; loadProofProfile() bumps the
    // generation so switching ICC profiles rebuilds the transform.  We leak
    // superseded handles into process lifetime (bounded: one transform per
    // loaded profile per slot) to avoid cross-thread destruction complexity.
    const int gi = withGamutCheck ? 1 : 0;
    const int ii = std::min(3, std::max(0, static_cast<int>(intent)));
    const uint64_t gen = cacheGeneration_;
    auto& slot = proofCache_[gi][ii];
    void* xform = nullptr;
    {
        std::shared_lock<std::shared_mutex> sl(slot.mutex);
        if (slot.transform && slot.generation == gen)
            xform = slot.transform;
    }
    if (!xform) {
        std::unique_lock<std::shared_mutex> ul(slot.mutex);
        if (!slot.transform || slot.generation != gen) {
            const cmsUInt32Number baseFlags = cmsFLAGS_NOCACHE | cmsFLAGS_BLACKPOINTCOMPENSATION |
                                              (withGamutCheck ? cmsFLAGS_GAMUTCHECK : 0);
            slot.transform = cmsCreateProofingTransformTHR(
                reinterpret_cast<cmsContext>(ctx_),
                reinterpret_cast<cmsHPROFILE>(prophoto_), TYPE_RGB_FLT,
                reinterpret_cast<cmsHPROFILE>(proofProfile_), TYPE_RGB_8,
                reinterpret_cast<cmsHPROFILE>(srgb_),
                intentCms(intent),
                INTENT_RELATIVE_COLORIMETRIC,
                baseFlags);
            slot.generation = gen;
        }
        xform = slot.transform;
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
    // marked as alarm code" into a F32 weight mask. lcms replaces out-of-gamut
    // pixels with the alarm colour rather than exposing a separate gamut flag,
    // so the mask is derived by matching that colour — which is why it must be
    // the *configured* one and not the former hard-coded pure red: with any
    // other colour the mask used to come back empty.
    const GamutAlarmCodes alarm = gamutAlarmCodes();
    auto proof = proofToDisplay(input, intent, true);
    if (!proof) return nullptr;
    const int w = proof->width(), h = proof->height();
    auto mask = ImageBuffer::create(w, h, PixelFormat::F32);
    const uint8_t* s = proof->data().data();
    float* d = reinterpret_cast<float*>(mask->data().data());
    // lcms writes the alarm codes into the 8-bit output exactly (0..255), but a
    // small tolerance absorbs any rounding in a different output path.
    const auto isAlarm = [](uint8_t v, uint8_t target) {
        const int diff = static_cast<int>(v) - static_cast<int>(target);
        return diff >= -2 && diff <= 2;
    };
    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n; ++i) {
        const size_t si = i * 3;
        const bool outOfGamut = isAlarm(s[si], alarm.r) &&
                                isAlarm(s[si + 1], alarm.g) &&
                                isAlarm(s[si + 2], alarm.b);
        const float v = outOfGamut ? 1.0f : 0.0f;
        d[si    ] = v;
        d[si + 1] = v;
        d[si + 2] = v;
    }
    return mask;
}

} // namespace lrc
