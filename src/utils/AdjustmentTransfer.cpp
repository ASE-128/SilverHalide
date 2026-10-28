#include "utils/AdjustmentTransfer.h"

#include "engine/Adjustments.h"

namespace lrc {
namespace {

bool hasCategory(AdjustmentCategory value,
                 AdjustmentCategory category) noexcept {
    return (value & category) != AdjustmentCategory::None;
}

} // namespace

void copyAdjustmentCategories(const Adjustments& source, Adjustments& target,
                              AdjustmentCategory categories) {
    if (hasCategory(categories, AdjustmentCategory::Basic)) {
        target.exposure = source.exposure;
        target.contrast = source.contrast;
        target.highlights = source.highlights;
        target.shadows = source.shadows;
        target.whites = source.whites;
        target.blacks = source.blacks;
    }

    if (hasCategory(categories, AdjustmentCategory::WhiteBalance)) {
        target.temperature = source.temperature;
        target.tint = source.tint;
    }

    if (hasCategory(categories, AdjustmentCategory::ToneCurve))
        target.curve = source.curve;

    if (hasCategory(categories, AdjustmentCategory::Color)) {
        target.saturation = source.saturation;
        target.hsl = source.hsl;
        target.splitTone = source.splitTone;
        target.colorTargets = source.colorTargets;
    }

    if (hasCategory(categories, AdjustmentCategory::Detail)) {
        target.sharpness = source.sharpness;
        target.sharpnessRadius = source.sharpnessRadius;
        target.sharpnessDetail = source.sharpnessDetail;
        target.sharpnessMasking = source.sharpnessMasking;
        target.luminance = source.luminance;
        target.luminanceDetail = source.luminanceDetail;
        target.luminanceContrast = source.luminanceContrast;
        target.colorNoiseReduction = source.colorNoiseReduction;
        target.colorNoiseReductionSmoothness =
            source.colorNoiseReductionSmoothness;
    }

    if (hasCategory(categories, AdjustmentCategory::Effects)) {
        target.texture = source.texture;
        target.clarity = source.clarity;
        target.dehaze = source.dehaze;
        target.vibrance = source.vibrance;
        target.vignette = source.vignette;
        target.vignetteMidpoint = source.vignetteMidpoint;
        target.vignetteRoundness = source.vignetteRoundness;
        target.vignetteFeather = source.vignetteFeather;
        target.vignetteHighlights = source.vignetteHighlights;
        target.vignetteStyle = source.vignetteStyle;
        target.grain = source.grain;
        target.grainSize = source.grainSize;
        target.grainRoughness = source.grainRoughness;
    }

    if (hasCategory(categories, AdjustmentCategory::LensCorrections)) {
        target.distortion = source.distortion;
        target.lensfunEnabled = source.lensfunEnabled;
        target.lensfunCameraMaker = source.lensfunCameraMaker;
        target.lensfunCameraModel = source.lensfunCameraModel;
        target.lensfunLensMaker = source.lensfunLensMaker;
        target.lensfunLensModel = source.lensfunLensModel;
        target.lensfunFocalLengthMm = source.lensfunFocalLengthMm;
        target.lensfunAperture = source.lensfunAperture;
        target.lensfunCropFactor = source.lensfunCropFactor;
    }

    if (hasCategory(categories, AdjustmentCategory::Crop)) {
        target.cropRect = source.cropRect;
        target.straightenAngle = source.straightenAngle;
    }

    if (hasCategory(categories, AdjustmentCategory::Masks))
        target.masks = source.masks;

    if (hasCategory(categories, AdjustmentCategory::SpotRemoval))
        target.spotRemovals = source.spotRemovals;

    if (hasCategory(categories, AdjustmentCategory::AiDenoise))
        target.aiDenoise = source.aiDenoise;
}

} // namespace lrc
