#pragma once

// ---------------------------------------------------------------------------
// PHASE_13: AiMaskGenerator
//
// Produces per-pixel weight masks (float32, [0,1]) from an input image using
// a pretrained segmentation model.  Three targets are supported:
//
//     Subject   — "select the person/object in front" (RMBG-2 / U²Net)
//     Sky       — "select sky pixels"                      (U²Net-Sky)
//     Background — the logical inverse of the Subject mask.
//
// Output is a single-channel weight map, packed into a 3-channel F32 (R=G=B)
// ImageBuffer so the rest of the pipeline (Pipeline::combinedMaskWeight,
// brushWeightField, etc.) can treat it as a grey-scale mask weight field.
//
// Like AiDenoiseProcessor, falls back to a pure C++ heuristic mask when the
// real ONNX model is unavailable so tests and LRC_ENABLE_AI=OFF builds still
// produce a visual result the UI can overlay.
//
// Pure C++.  Engine/ — no Qt.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/Adjustments.h"   // AiMaskTarget, AiMaskParams
#include "engine/AiInference.h"
#include "engine/ImageBuffer.h"

namespace lrc {

/**
 * @brief Maps AiMaskTarget -> the model id registered in AiInferenceEngine.
 *        Kept free so callers (and tests) can choose which models to load
 *        without dragging in Qt or the ModelManager manifest parser.
 */
inline const char* defaultModelIdFor(AiMaskTarget t) {
    switch (t) {
        case AiMaskTarget::Subject:    return "rmbg2-subject";
        case AiMaskTarget::Sky:        return "u2net-sky";
        case AiMaskTarget::Background: return "rmbg2-subject";  // same model, invert
    }
    return "";
}

class AiMaskGenerator {
public:
    explicit AiMaskGenerator(AiInferenceEngine* engine);
    ~AiMaskGenerator();

    /**
     * @brief Generate a mask for image `input` (F32 ProPhoto or sRGB U8/U16).
     *        Returns a w×h F32 single-channel (packed R=G=B) weight buffer.
     *        Never returns null; worst case the mask is all 0.5 (the "no
     *        information" mid-grey) or the heuristic fallback.
     */
    ImageBufferPtr generate(const ImageBuffer& input,
                            const AiMaskParams& params,
                            const std::string& modelIdOverride = "");

    /**
     * @brief Pure C++ fallback mask generator.  The returned weight map is
     *        only a rough estimate (bright pixels / blue-channel sky
     *        heuristic / centre-bias) but it keeps screenshots green and
     *        lets the UI display a red overlay in every build.
     *   Subject:   2D Gaussian centred on the image midpoint (simple saliency).
     *   Sky:       upper-third of image with high B/R ratio (blue sky clue).
     *   Background: inverted subject saliency.
     */
    static ImageBufferPtr heuristicFallback(const ImageBuffer& input,
                                            AiMaskTarget target);

    /** Apply threshold + feather (box-blur sigma) to an existing F32 mask. */
    static ImageBufferPtr refineMask(const ImageBuffer& maskFloat,
                                     float threshold, float featherSigma,
                                     bool invert);
private:
    AiInferenceEngine* engine_;
};

} // namespace lrc
