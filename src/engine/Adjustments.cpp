#include "engine/Adjustments.h"

#include <bit>
#include <cstring>

#include "utils/MathUtils.h"

namespace lrc {

bool Adjustments::operator==(const Adjustments& other) const {
    return exposure == other.exposure && contrast == other.contrast &&
           highlights == other.highlights && shadows == other.shadows &&
           whites == other.whites && blacks == other.blacks &&
           temperature == other.temperature && tint == other.tint &&
           saturation == other.saturation && texture == other.texture &&
           clarity == other.clarity && dehaze == other.dehaze &&
           vibrance == other.vibrance && sharpness == other.sharpness &&
           sharpnessRadius == other.sharpnessRadius &&
           sharpnessDetail == other.sharpnessDetail &&
           sharpnessMasking == other.sharpnessMasking &&
           luminance == other.luminance &&
           luminanceDetail == other.luminanceDetail &&
           luminanceContrast == other.luminanceContrast &&
           colorNoiseReduction == other.colorNoiseReduction &&
           colorNoiseReductionSmoothness == other.colorNoiseReductionSmoothness &&
           distortion == other.distortion && vignette == other.vignette &&
           lensfunEnabled == other.lensfunEnabled &&
           lensfunCameraMaker == other.lensfunCameraMaker &&
           lensfunCameraModel == other.lensfunCameraModel &&
           lensfunLensMaker == other.lensfunLensMaker &&
           lensfunLensModel == other.lensfunLensModel &&
           lensfunFocalLengthMm == other.lensfunFocalLengthMm &&
           lensfunAperture == other.lensfunAperture &&
           lensfunCropFactor == other.lensfunCropFactor &&
           vignetteMidpoint == other.vignetteMidpoint &&
           vignetteRoundness == other.vignetteRoundness &&
           vignetteFeather == other.vignetteFeather &&
           vignetteHighlights == other.vignetteHighlights &&
           vignetteStyle == other.vignetteStyle &&
           grain == other.grain && grainSize == other.grainSize &&
           grainRoughness == other.grainRoughness &&
           curve == other.curve && hsl == other.hsl &&
           splitTone == other.splitTone && masks == other.masks &&
           colorTargets == other.colorTargets &&
           spotRemovals == other.spotRemovals &&
           aiDenoise == other.aiDenoise;
}

void Adjustments::reset() { *this = Adjustments::defaults(); }

Adjustments Adjustments::defaults() {
    return Adjustments{};
}

namespace {

// Mixes the 64-bit payload of a double into the hash state.
inline void mixDouble(size_t& h, double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    // Normalize -0.0 and +0.0 so identical values always hash identically.
    if (v == 0.0)
        bits = 0;
    h = hashCombine(h, bits);
    h = hashCombine(h, bits >> 32);
}

// Mixes a normalized [0,1] float into a hash state.
inline void mixFloat(size_t& h, float v) {
    h = hashCombine(h, std::bit_cast<uint32_t>(v));
    h = hashCombine(h, static_cast<size_t>(v * 1048576.0f));
}

} // namespace

bool Mask::operator==(const Mask& other) const {
    return type == other.type && linear == other.linear && radial == other.radial &&
           brush == other.brush && invert == other.invert && opacity == other.opacity &&
           exposure == other.exposure && contrast == other.contrast &&
           highlights == other.highlights && shadows == other.shadows &&
           whites == other.whites && blacks == other.blacks &&
           clarity == other.clarity && dehaze == other.dehaze &&
           saturation == other.saturation && temperature == other.temperature &&
           tint == other.tint && color == other.color &&
           useColorRange == other.useColorRange &&
           colorRefR == other.colorRefR && colorRefG == other.colorRefG &&
           colorRefB == other.colorRefB && colorRange == other.colorRange &&
           colorSmoothness == other.colorSmoothness &&
           useLuminance == other.useLuminance &&
           luminanceRef == other.luminanceRef &&
           luminanceRange == other.luminanceRange &&
           luminanceSmoothness == other.luminanceSmoothness &&
           maskUnion == other.maskUnion &&
           ai == other.ai;
}

size_t Mask::hash() const {
    size_t h = 1469598103934665603ull;
    h = hashCombine(h, static_cast<size_t>(type));
    h = hashCombine(h, static_cast<size_t>(color.hasReference));
    mixFloat(h, color.refR);
    mixFloat(h, color.refG);
    mixFloat(h, color.refB);
    mixDouble(h, color.hueTolerance);
    mixDouble(h, color.satTolerance);
    mixDouble(h, color.lumaTolerance);
    mixDouble(h, color.softness);
    mixFloat(h, linear.startX);
    mixFloat(h, linear.startY);
    mixFloat(h, linear.endX);
    mixFloat(h, linear.endY);
    mixFloat(h, linear.feather);
    mixFloat(h, radial.centerX);
    mixFloat(h, radial.centerY);
    mixFloat(h, radial.radiusX);
    mixFloat(h, radial.radiusY);
    mixFloat(h, radial.feather);
    h = hashCombine(h, static_cast<size_t>(radial.invert));
    h = hashCombine(h, static_cast<size_t>(invert));
    mixDouble(h, opacity);
    h = hashCombine(h, brush.size());
    for (const BrushDab& d : brush) {
        mixFloat(h, d.x);
        mixFloat(h, d.y);
        mixFloat(h, d.radius);
        mixFloat(h, d.softness);
    }
    mixDouble(h, exposure);
    mixDouble(h, contrast);
    mixDouble(h, highlights);
    mixDouble(h, shadows);
    mixDouble(h, whites);
    mixDouble(h, blacks);
    mixDouble(h, clarity);
    mixDouble(h, dehaze);
    mixDouble(h, saturation);
    mixDouble(h, temperature);
    mixDouble(h, tint);
    // Sub-mask modifiers (they shape the weight field).
    h = hashCombine(h, static_cast<size_t>(useColorRange));
    mixFloat(h, colorRefR);
    mixFloat(h, colorRefG);
    mixFloat(h, colorRefB);
    mixFloat(h, colorRange);
    mixFloat(h, colorSmoothness);
    h = hashCombine(h, static_cast<size_t>(useLuminance));
    mixFloat(h, luminanceRef);
    mixFloat(h, luminanceRange);
    mixFloat(h, luminanceSmoothness);
    h = hashCombine(h, static_cast<size_t>(maskUnion));
    // AI mask target / threshold / feather / invert / model selection
    // all shape the weight raster (and thus the final image value via the
    // local-adjustment blend — include them in the full Mask::hash).
    h = hashCombine(h, static_cast<size_t>(ai.target));
    mixFloat(h, ai.threshold);
    mixFloat(h, ai.feather);
    h = hashCombine(h, static_cast<size_t>(ai.invert));
    h = hashCombine(h, static_cast<size_t>(ai.modelInputSize));
    h = hashCombine(h, static_cast<size_t>(ai.cachedWeightVersion));
    h = hashCombine(h, ai.modelIdOverride.size());
    for (char c : ai.modelIdOverride) h = hashCombine(h, static_cast<size_t>(c));
    // Click-prompt points shape the Prompt mask raster.
    h = hashCombine(h, ai.points.size());
    for (const SamPoint& pt : ai.points) {
        mixFloat(h, pt.nx);
        mixFloat(h, pt.ny);
        h = hashCombine(h, static_cast<size_t>(pt.label));
    }
    return h;
}

size_t Mask::geometryHash() const {
    // Geometry-only hash: type, linear/radial/brush shape, and both invert
    // flags. Intentionally omits opacity and the local-adjustment values so a
    // slider drag (which changes only those fields) keeps the cached weight
    // field valid. Brush dabs are hashed in order because the max-composition
    // is order-independent in value but the sequence is part of the geometry.
    size_t h = 1469598103934665603ull;
    h = hashCombine(h, static_cast<size_t>(type));
    h = hashCombine(h, static_cast<size_t>(color.hasReference));
    mixFloat(h, color.refR);
    mixFloat(h, color.refG);
    mixFloat(h, color.refB);
    mixDouble(h, color.hueTolerance);
    mixDouble(h, color.satTolerance);
    mixDouble(h, color.lumaTolerance);
    mixDouble(h, color.softness);
    mixFloat(h, linear.startX);
    mixFloat(h, linear.startY);
    mixFloat(h, linear.endX);
    mixFloat(h, linear.endY);
    mixFloat(h, linear.feather);
    mixFloat(h, radial.centerX);
    mixFloat(h, radial.centerY);
    mixFloat(h, radial.radiusX);
    mixFloat(h, radial.radiusY);
    mixFloat(h, radial.feather);
    h = hashCombine(h, static_cast<size_t>(radial.invert));
    h = hashCombine(h, static_cast<size_t>(invert));
    h = hashCombine(h, brush.size());
    for (const BrushDab& d : brush) {
        mixFloat(h, d.x);
        mixFloat(h, d.y);
        mixFloat(h, d.radius);
        mixFloat(h, d.softness);
    }
    // Sub-mask modifiers shape the weight field, so they belong to the
    // geometry key: a reference / range / smoothness / mode change must
    // invalidate the cached geometry field AND the viewport overlay.
    h = hashCombine(h, static_cast<size_t>(useColorRange));
    mixFloat(h, colorRefR);
    mixFloat(h, colorRefG);
    mixFloat(h, colorRefB);
    mixFloat(h, colorRange);
    mixFloat(h, colorSmoothness);
    h = hashCombine(h, static_cast<size_t>(useLuminance));
    mixFloat(h, luminanceRef);
    mixFloat(h, luminanceRange);
    mixFloat(h, luminanceSmoothness);
    h = hashCombine(h, static_cast<size_t>(maskUnion));
    // AI mask geometry.  target/threshold/feather/invert, the model override and
    // the click-prompt points all contribute to the per-pixel weight raster, so
    // they belong to the key even though AI weights are produced by
    // Pipeline::aiMaskWeightField rather than brushWeightField. The viewport
    // overlay memoises on geometryHash() for the same reason.
    h = hashCombine(h, static_cast<size_t>(ai.target));
    mixFloat(h, ai.threshold);
    mixFloat(h, ai.feather);
    h = hashCombine(h, static_cast<size_t>(ai.invert));
    h = hashCombine(h, static_cast<size_t>(ai.modelInputSize));
    h = hashCombine(h, static_cast<size_t>(ai.cachedWeightVersion));
    h = hashCombine(h, ai.modelIdOverride.size());
    for (char c : ai.modelIdOverride) h = hashCombine(h, static_cast<size_t>(c));
    // Prompt points are geometry: adding/removing/re-labelling one invalidates
    // the Prompt mask raster and the viewport overlay.
    h = hashCombine(h, ai.points.size());
    for (const SamPoint& pt : ai.points) {
        mixFloat(h, pt.nx);
        mixFloat(h, pt.ny);
        h = hashCombine(h, static_cast<size_t>(pt.label));
    }
    return h;
}

size_t ColorTarget::hash() const {
    size_t h = 1469598103934665603ull;
    h = hashCombine(h, static_cast<size_t>(enabled));
    mixFloat(h, refR);
    mixFloat(h, refG);
    mixFloat(h, refB);
    mixDouble(h, hueShift);
    mixDouble(h, saturation);
    mixDouble(h, luminance);
    mixDouble(h, range);
    mixDouble(h, smoothness);
    mixDouble(h, amount);
    return h;
}

size_t Adjustments::hash() const {
    size_t h = 1469598103934665603ull;
    mixDouble(h, exposure);
    mixDouble(h, contrast);
    mixDouble(h, highlights);
    mixDouble(h, shadows);
    mixDouble(h, whites);
    mixDouble(h, blacks);
    mixDouble(h, temperature);
    mixDouble(h, tint);
    mixDouble(h, saturation);
    mixDouble(h, texture);
    mixDouble(h, clarity);
    mixDouble(h, dehaze);
    mixDouble(h, vibrance);
    mixDouble(h, sharpness);
    mixDouble(h, sharpnessRadius);
    mixDouble(h, sharpnessDetail);
    mixDouble(h, sharpnessMasking);
    mixDouble(h, luminance);
    mixDouble(h, luminanceDetail);
    mixDouble(h, luminanceContrast);
    mixDouble(h, colorNoiseReduction);
    mixDouble(h, colorNoiseReductionSmoothness);
    mixDouble(h, distortion);
    mixDouble(h, vignette);
    h = hashCombine(h, static_cast<size_t>(lensfunEnabled));
    h = hashCombine(h, std::hash<std::string>{}(lensfunCameraMaker));
    h = hashCombine(h, std::hash<std::string>{}(lensfunCameraModel));
    h = hashCombine(h, std::hash<std::string>{}(lensfunLensMaker));
    h = hashCombine(h, std::hash<std::string>{}(lensfunLensModel));
    mixFloat(h, lensfunFocalLengthMm);
    mixFloat(h, lensfunAperture);
    mixFloat(h, lensfunCropFactor);
    mixDouble(h, vignetteMidpoint);
    mixDouble(h, vignetteRoundness);
    mixDouble(h, vignetteFeather);
    mixDouble(h, vignetteHighlights);
    h = hashCombine(h, static_cast<size_t>(vignetteStyle));
    mixDouble(h, grain);
    mixDouble(h, grainSize);
    mixDouble(h, grainRoughness);
    h = hashCombine(h, curve.size());
    for (const ToneCurvePoint& p : curve) {
        h = hashCombine(h, std::bit_cast<uint32_t>(p.x));
        h = hashCombine(h, std::bit_cast<uint32_t>(p.y));
    }
    // Per-channel HSL.
    for (const auto& ch : hsl) {
        mixDouble(h, ch.hue);
        mixDouble(h, ch.saturation);
        mixDouble(h, ch.luminance);
    }
    // Split toning.
    mixDouble(h, splitTone.highlightHue);
    mixDouble(h, splitTone.highlightSaturation);
    mixDouble(h, splitTone.shadowHue);
    mixDouble(h, splitTone.shadowSaturation);
    mixDouble(h, splitTone.balance);
    // Masked local adjustments.
    h = hashCombine(h, masks.size());
    for (const Mask& m : masks)
        h = hashCombine(h, m.hash());
    // Point-colour targets.
    h = hashCombine(h, colorTargets.size());
    for (const ColorTarget& ct : colorTargets)
        h = hashCombine(h, ct.hash());
    // Spot removals. Every field participates so moving/resizing a
    // spot or toggling its type/visibility invalidates the LRU cache key.
    h = hashCombine(h, spotRemovals.size());
    for (const SpotRemoval& s : spotRemovals) {
        h = hashCombine(h, static_cast<size_t>(s.type));
        mixFloat(h, s.sourceX);
        mixFloat(h, s.sourceY);
        mixFloat(h, s.targetX);
        mixFloat(h, s.targetY);
        mixFloat(h, s.radius);
        h = hashCombine(h, static_cast<size_t>(s.visible));
    }
    // AI denoise parameters — global participation in the overall
    // cache key (see hashStageE for the stage-specific participation).
    h = hashCombine(h, static_cast<size_t>(aiDenoise.enabled));
    mixFloat(h, aiDenoise.strength);
    h = hashCombine(h, static_cast<size_t>(aiDenoise.mode));
    h = hashCombine(h, static_cast<size_t>(aiDenoise.modelVersion));
    return h;
}

size_t Adjustments::hashStageA() const {
    size_t h = 1469598103934665603ull;
    mixDouble(h, exposure);
    mixDouble(h, contrast);
    mixDouble(h, highlights);
    mixDouble(h, shadows);
    mixDouble(h, whites);
    mixDouble(h, blacks);
    return h;
}

size_t Adjustments::hashStageB() const {
    size_t h = 1469598103934665603ull;
    h = hashCombine(h, curve.size());
    for (const ToneCurvePoint& p : curve) {
        h = hashCombine(h, std::bit_cast<uint32_t>(p.x));
        h = hashCombine(h, std::bit_cast<uint32_t>(p.y));
    }
    return h;
}

size_t Adjustments::hashStageC() const {
    size_t h = 1469598103934665603ull;
    mixDouble(h, temperature);
    mixDouble(h, tint);
    mixDouble(h, dehaze);
    mixDouble(h, vibrance);
    return h;
}

size_t Adjustments::hashStageD() const {
    size_t h = 1469598103934665603ull;
    mixDouble(h, saturation);
    for (const auto& ch : hsl) {
        mixDouble(h, ch.hue);
        mixDouble(h, ch.saturation);
        mixDouble(h, ch.luminance);
    }
    mixDouble(h, splitTone.highlightHue);
    mixDouble(h, splitTone.highlightSaturation);
    mixDouble(h, splitTone.shadowHue);
    mixDouble(h, splitTone.shadowSaturation);
    mixDouble(h, splitTone.balance);
    h = hashCombine(h, colorTargets.size());
    for (const ColorTarget& ct : colorTargets)
        h = hashCombine(h, ct.hash());
    return h;
}

size_t Adjustments::hashStageE() const {
    size_t h = 1469598103934665603ull;
    mixDouble(h, texture);
    mixDouble(h, clarity);
    mixDouble(h, sharpness);
    mixDouble(h, sharpnessRadius);
    mixDouble(h, sharpnessDetail);
    mixDouble(h, sharpnessMasking);
    mixDouble(h, luminance);
    mixDouble(h, luminanceDetail);
    mixDouble(h, luminanceContrast);
    mixDouble(h, colorNoiseReduction);
    mixDouble(h, colorNoiseReductionSmoothness);
    mixDouble(h, distortion);
    mixDouble(h, vignette);
    mixDouble(h, vignetteMidpoint);
    mixDouble(h, vignetteRoundness);
    mixDouble(h, vignetteFeather);
    mixDouble(h, vignetteHighlights);
    h = hashCombine(h, static_cast<size_t>(vignetteStyle));
    mixDouble(h, grain);
    mixDouble(h, grainSize);
    mixDouble(h, grainRoughness);
    h = hashCombine(h, masks.size());
    for (const Mask& m : masks)
        h = hashCombine(h, m.hash());
    // Spot removals run in the spatial stage, so they belong to the
    // stage-E key: adding/moving/resizing a spot restarts the incremental
    // preview render from stage E without recomputing the tonal stages A-D.
    h = hashCombine(h, spotRemovals.size());
    for (const SpotRemoval& s : spotRemovals) {
        h = hashCombine(h, static_cast<size_t>(s.type));
        mixFloat(h, s.sourceX);
        mixFloat(h, s.sourceY);
        mixFloat(h, s.targetX);
        mixFloat(h, s.targetY);
        mixFloat(h, s.radius);
        h = hashCombine(h, static_cast<size_t>(s.visible));
    }
    // AI denoise is a spatial-stage operation (it runs after the
    // manual NR inside the spatial kernel block), so any change in its
    // parameters should trigger a stage-E restart in the incremental cache.
    h = hashCombine(h, static_cast<size_t>(aiDenoise.enabled));
    mixFloat(h, aiDenoise.strength);
    h = hashCombine(h, static_cast<size_t>(aiDenoise.mode));
    h = hashCombine(h, static_cast<size_t>(aiDenoise.modelVersion));
    return h;
}

size_t Adjustments::hashStageF() {
    // Stage F (sRGB output) carries no adjustment parameters; a fixed constant
    // lets the incremental render treat it as an always-unchanged stage.
    return 0x5F5F5F5Fu;
}

} // namespace lrc
