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

void Mask::setType(MaskType t) {
    type = t;
    switch (t) {
        case MaskType::LinearGradient:
            if (!std::holds_alternative<LinearGradientMask>(shape))
                shape.emplace<LinearGradientMask>();
            break;
        case MaskType::RadialGradient:
            if (!std::holds_alternative<RadialGradientMask>(shape))
                shape.emplace<RadialGradientMask>();
            break;
        case MaskType::Brush:
            if (!std::holds_alternative<BrushGeometry>(shape))
                shape.emplace<BrushGeometry>();
            break;
        case MaskType::ColorRange:
            if (!std::holds_alternative<ColorRangeMask>(shape))
                shape.emplace<ColorRangeMask>();
            break;
        case MaskType::AiMask:
            if (!std::holds_alternative<AiMaskParams>(shape))
                shape.emplace<AiMaskParams>();
            break;
        case MaskType::None:
            if (!std::holds_alternative<std::monostate>(shape))
                shape.emplace<std::monostate>();
            break;
    }
}

bool Mask::shapeMatchesType() const noexcept {
    switch (type) {
        case MaskType::LinearGradient:
            return std::holds_alternative<LinearGradientMask>(shape);
        case MaskType::RadialGradient:
            return std::holds_alternative<RadialGradientMask>(shape);
        case MaskType::Brush:
            return std::holds_alternative<BrushGeometry>(shape);
        case MaskType::ColorRange:
            return std::holds_alternative<ColorRangeMask>(shape);
        case MaskType::AiMask:
            return std::holds_alternative<AiMaskParams>(shape);
        case MaskType::None:
            return std::holds_alternative<std::monostate>(shape);
    }
    return false;
}

void Mask::copyGeometryFrom(const Mask& other) {
    type = other.type;
    invert = other.invert;
    // Mirrors the pre-variant field-by-field geometry sync: only the geometry
    // alternatives are copied. Colour-range / AI parameters are edited through
    // their own paths and are deliberately left alone.
    if (const auto* lin = std::get_if<LinearGradientMask>(&other.shape))
        shape = *lin;
    else if (const auto* rad = std::get_if<RadialGradientMask>(&other.shape))
        shape = *rad;
    else if (const auto* geo = std::get_if<BrushGeometry>(&other.shape))
        shape = *geo;
}

bool Mask::operator==(const Mask& other) const {
    // Field set and order are unchanged from the pre-variant implementation.
    // An unused geometry compares as its default-constructed value, which is
    // exactly what the old unused member held, so equality is preserved for
    // every state the pipeline can produce.
    return type == other.type && linear() == other.linear() &&
           radial() == other.radial() && brush() == other.brush() &&
           invert == other.invert && opacity == other.opacity &&
           exposure == other.exposure && contrast == other.contrast &&
           highlights == other.highlights && shadows == other.shadows &&
           whites == other.whites && blacks == other.blacks &&
           clarity == other.clarity && dehaze == other.dehaze &&
           saturation == other.saturation && temperature == other.temperature &&
           tint == other.tint && color() == other.color() &&
           useColorRange == other.useColorRange &&
           colorRefR == other.colorRefR && colorRefG == other.colorRefG &&
           colorRefB == other.colorRefB && colorRange == other.colorRange &&
           colorSmoothness == other.colorSmoothness &&
           useLuminance == other.useLuminance &&
           luminanceRef == other.luminanceRef &&
           luminanceRange == other.luminanceRange &&
           luminanceSmoothness == other.luminanceSmoothness &&
           maskUnion == other.maskUnion &&
           ai() == other.ai();
}

size_t Mask::hash() const noexcept {
    size_t h = 1469598103934665603ull;
    h = hashCombine(h, static_cast<size_t>(type));
    // Every geometry participates -- resident or not -- in exactly the order of
    // the pre-variant implementation. A non-resident geometry contributes the
    // default-constructed value of its alternative, which is bit-identical to
    // the default member the old struct always carried, so the cache key (and
    // the mask oracle's mask.hash.* rows) stay stable.
    const ColorRangeMask& cr = color();
    h = hashCombine(h, static_cast<size_t>(cr.hasReference));
    mixFloat(h, cr.refR);
    mixFloat(h, cr.refG);
    mixFloat(h, cr.refB);
    mixDouble(h, cr.hueTolerance);
    mixDouble(h, cr.satTolerance);
    mixDouble(h, cr.lumaTolerance);
    mixDouble(h, cr.softness);
    const LinearGradientMask& lg = linear();
    mixFloat(h, lg.startX);
    mixFloat(h, lg.startY);
    mixFloat(h, lg.endX);
    mixFloat(h, lg.endY);
    mixFloat(h, lg.feather);
    const RadialGradientMask& rg = radial();
    mixFloat(h, rg.centerX);
    mixFloat(h, rg.centerY);
    mixFloat(h, rg.radiusX);
    mixFloat(h, rg.radiusY);
    mixFloat(h, rg.feather);
    h = hashCombine(h, static_cast<size_t>(rg.invert));
    h = hashCombine(h, static_cast<size_t>(invert));
    mixDouble(h, opacity);
    const std::vector<BrushDab>& dabs = brush();
    h = hashCombine(h, dabs.size());
    for (const BrushDab& d : dabs) {
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
    const AiMaskParams& aip = ai();
    h = hashCombine(h, static_cast<size_t>(aip.target));
    mixFloat(h, aip.threshold);
    mixFloat(h, aip.feather);
    h = hashCombine(h, static_cast<size_t>(aip.invert));
    h = hashCombine(h, static_cast<size_t>(aip.modelInputSize));
    h = hashCombine(h, static_cast<size_t>(aip.cachedWeightVersion));
    h = hashCombine(h, aip.modelIdOverride.size());
    for (char c : aip.modelIdOverride) h = hashCombine(h, static_cast<size_t>(c));
    // Click-prompt points shape the Prompt mask raster.
    h = hashCombine(h, aip.points.size());
    for (const SamPoint& pt : aip.points) {
        mixFloat(h, pt.nx);
        mixFloat(h, pt.ny);
        h = hashCombine(h, static_cast<size_t>(pt.label));
    }
    return h;
}

size_t Mask::geometryHash() const noexcept {
    // Geometry-only hash: type, linear/radial/brush shape, and both invert
    // flags. Intentionally omits opacity and the local-adjustment values so a
    // slider drag (which changes only those fields) keeps the cached weight
    // field valid. Brush dabs are hashed in order because the max-composition
    // is order-independent in value but the sequence is part of the geometry.
    // Field set and mixing order are unchanged from the pre-variant version;
    // a non-resident alternative contributes its default-constructed value.
    size_t h = 1469598103934665603ull;
    h = hashCombine(h, static_cast<size_t>(type));
    const ColorRangeMask& cr = color();
    h = hashCombine(h, static_cast<size_t>(cr.hasReference));
    mixFloat(h, cr.refR);
    mixFloat(h, cr.refG);
    mixFloat(h, cr.refB);
    mixDouble(h, cr.hueTolerance);
    mixDouble(h, cr.satTolerance);
    mixDouble(h, cr.lumaTolerance);
    mixDouble(h, cr.softness);
    const LinearGradientMask& lg = linear();
    mixFloat(h, lg.startX);
    mixFloat(h, lg.startY);
    mixFloat(h, lg.endX);
    mixFloat(h, lg.endY);
    mixFloat(h, lg.feather);
    const RadialGradientMask& rg = radial();
    mixFloat(h, rg.centerX);
    mixFloat(h, rg.centerY);
    mixFloat(h, rg.radiusX);
    mixFloat(h, rg.radiusY);
    mixFloat(h, rg.feather);
    h = hashCombine(h, static_cast<size_t>(rg.invert));
    h = hashCombine(h, static_cast<size_t>(invert));
    const std::vector<BrushDab>& dabs = brush();
    h = hashCombine(h, dabs.size());
    for (const BrushDab& d : dabs) {
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
    const AiMaskParams& aip = ai();
    h = hashCombine(h, static_cast<size_t>(aip.target));
    mixFloat(h, aip.threshold);
    mixFloat(h, aip.feather);
    h = hashCombine(h, static_cast<size_t>(aip.invert));
    h = hashCombine(h, static_cast<size_t>(aip.modelInputSize));
    h = hashCombine(h, static_cast<size_t>(aip.cachedWeightVersion));
    h = hashCombine(h, aip.modelIdOverride.size());
    for (char c : aip.modelIdOverride) h = hashCombine(h, static_cast<size_t>(c));
    // Prompt points are geometry: adding/removing/re-labelling one invalidates
    // the Prompt mask raster and the viewport overlay.
    h = hashCombine(h, aip.points.size());
    for (const SamPoint& pt : aip.points) {
        mixFloat(h, pt.nx);
        mixFloat(h, pt.ny);
        h = hashCombine(h, static_cast<size_t>(pt.label));
    }
    return h;
}

size_t ColorTarget::hash() const noexcept {
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

size_t Adjustments::hash() const noexcept {
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

size_t Adjustments::hashStageA() const noexcept {
    size_t h = 1469598103934665603ull;
    mixDouble(h, exposure);
    mixDouble(h, contrast);
    mixDouble(h, highlights);
    mixDouble(h, shadows);
    mixDouble(h, whites);
    mixDouble(h, blacks);
    return h;
}

size_t Adjustments::hashStageB() const noexcept {
    size_t h = 1469598103934665603ull;
    h = hashCombine(h, curve.size());
    for (const ToneCurvePoint& p : curve) {
        h = hashCombine(h, std::bit_cast<uint32_t>(p.x));
        h = hashCombine(h, std::bit_cast<uint32_t>(p.y));
    }
    return h;
}

size_t Adjustments::hashStageC() const noexcept {
    size_t h = 1469598103934665603ull;
    mixDouble(h, temperature);
    mixDouble(h, tint);
    mixDouble(h, dehaze);
    mixDouble(h, vibrance);
    return h;
}

size_t Adjustments::hashStageD() const noexcept {
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

size_t Adjustments::hashStageE() const noexcept {
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

size_t Adjustments::hashStageF() noexcept {
    // Stage F (sRGB output) carries no adjustment parameters; a fixed constant
    // lets the incremental render treat it as an always-unchanged stage.
    return 0x5F5F5F5Fu;
}

} // namespace lrc
