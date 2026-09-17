#pragma once

// Non-destructive editing pipeline. Renders a decoded linear ProPhoto buffer
// through a fixed stack of adjustments entirely in linear light (gamma 1.0):
//
//     exposure -> contrast -> highlights -> shadows -> tone curve ->
//     white balance -> HSL (per-channel) -> split toning
//
// The final step converts the result to display-ready 8-bit sRGB through the
// colorManager. The input buffer is never modified.
//
// Previews are rendered at a bounded resolution and cached with an LRU keyed
// by (image id, adjustment hash, target size).

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "engine/Adjustments.h"
#include "engine/ImageBuffer.h"

namespace lrc {

class ColorManager;
class AiInferenceEngine;  // ONNX Runtime inference wrapper

// Engine-side render target size for preview-resolution rendering. Pure C++ so
// the engine stays Qt-free; the UI layer converts from QSize.
struct Size {
    int width = 0;
    int height = 0;
};

class Pipeline {
public:
    explicit Pipeline(std::shared_ptr<const ColorManager> colorManager);

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // Renders a preview whose longest side fits maxDim. When maxDim <= 0 the
    // full resolution is processed (export / 1:1). Results are cached.
    ImageBufferPtr render(uint64_t imageId, const ImageBuffer& linear,
                          const Adjustments& adjustments, int maxDim);
    ImageBufferPtr renderFull(uint64_t imageId, const ImageBuffer& linear,
                              const Adjustments& adjustments);
    // Renders at full resolution but stays in *linear ProPhoto* F32 space (no
    // sRGB conversion). Used for 16-bit TIFF and linear DNG export.
    ImageBufferPtr renderLinearFull(uint64_t imageId, const ImageBuffer& linear,
                                    const Adjustments& adjustments);
    // Renders at full resolution to display-ready sRGB U8, but does NOT write
    // the result into the LRU cache (renderFull would). This is the 1:1 view
    // entry point: a full-size U8 buffer is far too large for the preview LRU
    // and would evict every thumbnail.
    ImageBufferPtr renderFullDisplay(uint64_t imageId, const ImageBuffer& linear,
                                     const Adjustments& adjustments);

    // Preview-resolution render with per-stage incremental caching.
    // `source` is a linear ProPhoto buffer (F32 preferred; U16/U8 are converted);
    // it is downscaled (area-average, never upscaled) to fit `targetSize`.
    // `fullLongEdge` is the native full-resolution long edge used to scale the
    // absolute-pixel spatial kernels (texture/sharpening/NR/grain); 0 falls back
    // to the source's own long edge. `imageId` participates in the cache identity
    // (0 for callers without a catalog id). Returns display-ready sRGB U8.
    // The input is never modified.
    ImageBufferPtr renderPreview(const ImageBuffer& source,
                                 const Adjustments& adjustments,
                                 const Size& targetSize,
                                 int fullLongEdge = 0,
                                 uint64_t imageId = 0);

    // --- Incremental render-state controls ----------------------------------
    void clearIncrementalCache();
    void setIncrementalCacheCapacity(size_t capacityBytes);
    size_t incrementalCacheCapacity() const;
    size_t incrementalCacheBytes() const;

    // Test probes for the incremental cache.
    // 0-based stage at which the last renderPreview recomputed (A=0..F=5);
    // -1 when the last render was served entirely from the cache.
    int lastRecomputeStage() const;
    // 0-based stage whose cached output the last renderPreview reused as its
    // input (A=0..E=4); -1 when the last render started from the source.
    int lastReuseStartStage() const;

    // Test-only hook that drops a single retained intermediate stage from the
    // incremental cache so tests can exercise the "find-nearest-retained-
    // ancestor" backtrack branch of renderPreview. Silently ignores stages
    // outside [0..4] or stages that are not currently retained.
    void dropIncrementalStageForTest(int stage);

    // --- Preview-resolution kernel scaling ----------------------------------
    // sigma-scale factor for a preview whose working long edge is
    // `workingLongEdge` and whose native full-res long edge is `fullLongEdge`.
    // 0 / negative edges (unknown) -> 1.0 (no scaling); never exceeds 1.0.
    static float previewSigmaScale(int workingLongEdge, int fullLongEdge);

    // Effective spatial-kernel sizes (px) for a working buffer of w*h at the
    // given sigma scale. Single source of truth for applySpatialAdjustments and
    // its scaled (preview) variant, exposed so tests can verify the
    // preview/full-resolution scaling rule.
    struct SpatialKernels {
        float textureSigma = 0.0f;     // local-contrast radius (scaled)
        float clarityRadius = 0.0f;    // buffer-relative, NOT scaled
        float sharpnessSigma = 0.0f;   // unsharp radius (scaled)
        float lumaNRSigma = 0.0f;      // luma noise-reduction sigma (scaled)
        float lumaContrastSigma = 0.0f; // NR local-contrast restore sigma (scaled)
        float colorNRSigma = 0.0f;     // chroma noise-reduction sigma (scaled)
        float grainCell = 0.0f;        // film-grain cell size (scaled)
    };
    static SpatialKernels spatialKernels(const Adjustments& adjustments,
                                         int w, int h, float sigmaScale);

    void clearCache();
    void setCacheCapacity(size_t capacityBytes);

    // --- AI inference injection --------------------------------------------
    // Engine-wide ONNX inference handle. Set once at app startup (by
    // MainWindow) after ModelManager has verified/loaded bundled models from
    // <applicationDirPath>/models. When null (headless tests, LRC_ENABLE_AI=
    // OFF builds, or before MainWindow constructs the engine), the AI code
    // paths silently fall back to the existing pure-C++ implementations:
    //   • AiDenoiseProcessor::denoise  -> fallbackBilateral
    //   • AiMaskGenerator::generate    -> heuristicFallback
    // So the static applySpatialAdjustments / applyLocalAdjustments entry
    // points remain bit-identical to the pre-AI behaviour when no engine is
    // attached. The pointer is non-owning; the caller (MainWindow) owns the
    // AiInferenceEngine instance and must outlive every Pipeline render call.
    // Thread-safety: AiInferenceEngine::run / runImage are internally
    // thread-safe (per-thread ONNX sessions), so a single shared engine
    // serves both the PreviewWorker thread and the export thread.
    static AiInferenceEngine* globalAiEngine();
    static void setGlobalAiEngine(AiInferenceEngine* engine);

    // LRU cache budget and current usage in bytes (thread-safe).
    size_t cacheCapacityBytes() const;
    size_t cacheBytes() const;

    // Applies the adjustment stack to an F32 buffer in place (linear light).
    static void applyAdjustments(const Adjustments& adjustments,
                                 float* pixels, size_t pixelCount);

    // Applies the spatial stages (texture, clarity, sharpening, lens
    // distortion, vignette) to an F32 RGB buffer in place. Must run after
    // applyAdjustments; needs the buffer dimensions for neighborhood ops.
    static void applySpatialAdjustments(const Adjustments& adjustments,
                                        float* pixels, int width, int height);

    // Applies masked local adjustments. Each mask's own adjustments are blended
    // into the buffer with weight = maskWeight * opacity. Must run after
    // applyAdjustments; needs dimensions for the mask geometry.
    static void applyLocalAdjustments(const Adjustments& adjustments,
                                      float* pixels, int width, int height);

    // Applies spot removal / healing brush corrections to an F32 RGB
    // buffer in place, on the linear ProPhoto working buffer. Must run after
    // the spatial stages (applySpatialAdjustments) and before the sRGB
    // conversion; the normalized spot coordinates are mapped to pixels using
    // the given buffer dimensions, so the preview and export paths agree.
    // When adjustments.spotRemovals is empty this is a no-op (the neutral
    // parameter stack stays bit-exact).
    static void applySpotRemoval(const Adjustments& adjustments,
                                 float* pixels, int width, int height);

    // Returns the mask weight in [0,1] at normalized image coordinates
    // (nx, ny in [0,1]). Exposed for tests and for the Alt-preview overlay.
    static float maskWeight(const Mask& mask, int width, int height,
                            float nx, float ny);

    // Rasterizes the mask's per-pixel weight into a w*h buffer (values in
    // [0,1]). Entry y*w+x is bit-identical to maskWeight at the same sample
    // point (nx = x/(w-1), ny = y/(h-1)); the mask's invert is folded in.
    // Results are cached globally, keyed by (Mask::geometryHash, w, h), so a
    // brush stroke / local-slider change that leaves the geometry intact hits
    // the cache. Returns null only for non-positive dimensions.
    static std::shared_ptr<const std::vector<float>> brushWeightField(
        const Mask& mask, int width, int height);

    // Post-crop vignette weight field for the render buffer. Entry y*w+x is the
    // geometric vignette weight in [0,1] (0 at the buffer's geometric centre,
    // 1 at the corners). It depends only on midpoint/roundness/feather/style and
    // the buffer size -- NOT on amount/highlights -- so a pure amount change
    // reuses the cached field. Results are cached globally, keyed by those
    // parameters and w/h. Returns null only for non-positive
    // dimensions.
    static std::shared_ptr<const std::vector<float>>
    postCropVignetteWeightField(const Adjustments& adjustments, int width,
                                int height);

    // Per-pixel weight [0,1] for a colour-range mask, compared in HSL space.
    // r/g/b are linear RGB [0,1]. Exposed for tests and the UI weight overlay.
    static float colorRangeWeight(const ColorRangeMask& mask, float r, float g,
                                  float b);

    // --- Sub-mask weight helpers ---------------------------------------------
    // Per-pixel weight [0,1] of a geometry mask's colour-range modifier. The
    // distance is the max of normalized hue/sat/luma distances to the sampled
    // reference; `colorRange` sets the band width and `colorSmoothness` its
    // transition softness. Returns 1.0 (the identity element of the product)
    // when the modifier is disabled. r/g/b are linear ProPhoto RGB [0,1].
    static float colorSubMaskWeight(const Mask& mask, float r, float g, float b);

    // Per-pixel weight [0,1] of a geometry mask's luminance-range modifier:
    // |L - luminanceRef| with L = 0.2126R + 0.7152G + 0.0722B. `luminanceRange`
    // sets the band width, `luminanceSmoothness` its softness. Returns 1.0
    // when the modifier is disabled.
    static float luminanceSubMaskWeight(const Mask& mask, float r, float g,
                                        float b);

    // Combined final weight [0,1] at normalized image coordinates (nx, ny):
    // the geometry weight (invert folded in) combined with the enabled
    // colour/luminance sub-mask weights -- product for the default intersection
    // mode, max for union. With no sub-mask enabled this is exactly maskWeight,
    // so the pre-min/max combination behaviour is preserved. Exposed for tests
    // and the viewport's combined weight overlay.
    static float combinedMaskWeight(const Mask& mask, int width, int height,
                                    float nx, float ny, float r, float g,
                                    float b);

    // Computes an automatic exposure/contrast/vibrance suggestion by analysing
    // the linear buffer's luminance (mean -> mid-gray, percentile stretch,
    // low saturation boost). Returns defaults when the image is empty.
    static Adjustments autoAdjust(const ImageBuffer& linear);

    // White-balance estimate for a single linear-RGB sample (ProPhoto, [0,1]).
    // Both fields use the same units as Adjustments::temperature / tint (the
    // -100..+100 slider domain); 0 means the sample is already neutral.
    struct WhiteBalanceEstimate {
        double temperature = 0.0;
        double tint = 0.0;
    };

    // Grey-world estimate: the sample is assumed to have been a neutral grey,
    // so any cast is illumination. Returns the temperature/tint offsets that
    // neutralise it (the exact inverse of the pipeline's white-balance gains,
    // see estimateWhiteBalance in Pipeline.cpp for the derivation).
    static WhiteBalanceEstimate estimateWhiteBalance(float lr, float lg,
                                                     float lb);

    // --- LR-style Alt-key visualization ------------------------------------
    // While holding Alt over a slider, Lightroom replaces the preview with a
    // diagnostic view of that adjustment's effect. These modes mirror that
    // behaviour. The overlay is a display-ready U8 RGB buffer (maxDim bounds
    // its longest side; maxDim <= 0 means full resolution).
    enum class VisualizeMode : uint8_t {
        Clipping,    // red = clipped highlights, blue = clipped shadows
        DetailMask,  // B&W detail map (clarity/dehaze/texture effect)
        EdgeMask,    // B&W edge map (sharpening masking threshold)
    };

    // Renders the diagnostic overlay described by `mode` from the linear
    // working-space buffer. Returns an empty buffer on failure.
    ImageBufferPtr visualizeOverlay(const ImageBuffer& linear,
                                    const Adjustments& adjustments,
                                    VisualizeMode mode, int maxDim) const;

private:
    struct CacheKey {
        uint64_t imageId;
        uint64_t adjHash;
        int maxDim;

        bool operator==(const CacheKey& o) const {
            return imageId == o.imageId && adjHash == o.adjHash &&
                   maxDim == o.maxDim;
        }
    };
    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const noexcept {
            size_t h = 1469598103934665603ull;
            h = (h ^ k.imageId) * 1099511628211ull;
            h = (h ^ k.adjHash) * 1099511628211ull;
            h = (h ^ static_cast<size_t>(k.maxDim)) * 1099511628211ull;
            return h;
        }
    };

    // Renders to an F32 buffer in linear ProPhoto space; the caller decides
    // whether to convert to display sRGB afterwards.
    ImageBufferPtr renderInternalLinear(const ImageBuffer& linear,
                                        const Adjustments& adjustments,
                                        int maxDim) const;
    ImageBufferPtr renderInternal(const ImageBuffer& linear,
                                  const Adjustments& adjustments,
                                  int maxDim) const;
    bool lookupCache(const CacheKey& key, ImageBufferPtr& out);
    void insertCache(const CacheKey& key, const ImageBufferPtr& buffer);

    // --- Incremental render state -----------------------------------------
    // Stage outputs A..D (F32 linear) + F (U8 sRGB) are retained. Stage E
    // (spatial) is deliberately NOT retained: nothing downstream reads it (F is
    // the final conversion and always recomputes after E), so caching it would
    // only consume the memory budget. Reuse rule: find the first changed stage,
    // reuse every retained stage before it as input, recompute from there.
    struct RenderState {
        uint64_t sourceKey = 0;        // imageId + working size + content fingerprint
        uint64_t stageHashes[6] = {};  // last computed per-stage hashes
        ImageBufferPtr stages[6];      // 0..3 F32, 4 unused (E), 5 U8 (F)
        bool present[6] = {};          // which stages are currently retained
        uint32_t accessTick[6] = {};   // LRU timestamp per retained stage
        uint32_t tick = 0;
        size_t bytes = 0;              // sum of retained stage sizes
        bool valid = false;
    };
    void touchStageLocked(int stage);
    void evictIncrementalLocked();
    uint64_t computeSourceKey(const ImageBuffer& source, int workingW,
                              int workingH, int fullLongEdge,
                              uint64_t imageId) const;

    std::shared_ptr<const ColorManager> cm_;
    // Initialized in the ctor from lrc::app::kCacheCapacityMB (single source
    // of truth). ~40 previews at 1080p for the default 256 MB.
    size_t capacityBytes_;
    std::unordered_map<CacheKey, ImageBufferPtr, CacheKeyHash> cache_;
    std::list<CacheKey> lru_;
    size_t cacheBytes_ = 0;
    mutable std::mutex cacheMutex_;

    // Incremental preview render-state cache. Guarded by renderStateMutex_;
    // production use is single-threaded (the PreviewWorker thread), the mutex
    // protects the main thread's reads of the lastRecomputeStage probe.
    size_t incrementalBudgetBytes_ = 150u * 1024u * 1024u;
    RenderState renderState_;
    int lastRecomputeStage_ = -1;
    int lastReuseStartStage_ = -1;
    mutable std::mutex renderStateMutex_;

    // Non-owning pointer to the shared ONNX inference engine.
    // Defaults to null; set via setGlobalAiEngine at app startup. When null,
    // AI denoise/mask code paths degrade to their pure-C++ fallbacks.
    static AiInferenceEngine* aiEngine_;
};

} // namespace lrc
