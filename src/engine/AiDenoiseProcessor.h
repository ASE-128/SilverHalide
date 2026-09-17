#pragma once

// ---------------------------------------------------------------------------
// AiDenoiseProcessor
//
// Pure C++ denoise driver that wraps AiInferenceEngine to remove colour +
// luminance noise from an image buffer.  When ONNX Runtime / the denoise
// model is unavailable the class falls back to the existing manual NR paths
// already implemented in Pipeline::applySpatialAdjustments — callers get a
// reasonable (but not AI) result in every build.
//
// The core design is a 2-level pipeline:
//   1. Overlap-tiling: high-resolution images cannot be fed to the network
//      in one pass.  We split the image into TILE x TILE tiles with an
//      OVERLAP pixel feathered border on each side, run each tile through
//      the model, then feather-blend the overlaps back into the output so
//      there are no visible seams.
//   2. Strength blending: the final pixels are `src*(1-s) + denoised*s`.
//      Caching the denoised buffer (with strength==100%) means dragging the
//      "strength" slider is a 200ms mix rather than a full re-inference.
//
// Pure C++.  Engine/ — no Qt.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/AiInference.h"
#include "engine/ImageBuffer.h"

namespace lrc {

struct AiDenoiseOptions {
    float strength = 1.0f;       // 0..1 (linearised from the user's 0..100)
    bool luminanceOnly = false;  // if true only Y is touched after denoise
    int tileSize = 512;          // tile side in pixels
    int overlap = 64;            // overlap side in pixels (bilateral feather)
    // Colour-space hint:
    //   "prophoto"  — buffer is linear ProPhoto (our working space)
    //   "srgb"      — buffer is gamma sRGB
    // Some denoise models are trained on sRGB; we do the round-trip through
    // the ColorManager implicitly when a working-space pointer is passed in
    // the constructor below.
    const char* inputSpace = "prophoto";
};

/** Forward declaration of ColorManager (only a pointer needed here). */
class ColorManager;

class AiDenoiseProcessor {
public:
    /** Constructor. The engine pointer is borrowed; callers keep it alive. */
    explicit AiDenoiseProcessor(AiInferenceEngine* engine,
                                const ColorManager* cm = nullptr);
    ~AiDenoiseProcessor();

    /**
     * @brief Denoise an F32 image buffer in the ProPhoto working space.
     *
     * If the real ONNX denoise model (modelId) has been loaded by the engine,
     * runs tiled inference + feather-blend.  Otherwise falls back to a pure
     * bilateral-filter (reference C++ implementation) so in both builds the
     * output has reduced pixel variance.
     *
     * @param input      F32 image buffer (linear ProPhoto RGB, 3ch interleaved)
     * @param modelId    "nafnet-denoise" or another model registered with
     *                   the engine.  An empty id forces the fallback path.
     * @param opts       Strength / tile size / colour space hints.
     * @return Denoised F32 buffer (never null; at minimum a copy of input).
     */
    ImageBufferPtr denoise(const ImageBuffer& input,
                           const std::string& modelId,
                           const AiDenoiseOptions& opts);

    /** Tiled + feathered inference (exposed for unit tests).  `input` is an
     *  F32 buffer; the caller must already have registered `modelId`. */
    static ImageBufferPtr runTiledInference(AiInferenceEngine* engine,
                                            const ImageBuffer& input,
                                            const std::string& modelId,
                                            int tileSize, int overlap);

    /** Pure C++ fallback denoise: a small bilateral filter that smooths
     *  Gaussian noise while preserving strong edges.  Used as the stub when
     *  the real AI model is unavailable.  sigma controls strength. */
    static ImageBufferPtr fallbackBilateral(const ImageBuffer& input,
                                            float sigmaSpatial = 2.0f,
                                            float sigmaRange = 0.08f,
                                            int radius = 3);
    /** Blend two F32 buffers: out = a*(1-strength) + b*strength, per-pixel. */
    static ImageBufferPtr blend(const ImageBuffer& a,
                                const ImageBuffer& b,
                                float strength);

private:
    AiInferenceEngine* engine_;   // non-owning
    const ColorManager* cm_;      // non-owning (may be null)
};

} // namespace lrc
