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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

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

// RGB histogram bins: 3 channels x 256 display-encoded bins. Values are
// uint32 pixel counts; the whole buffer is traversed (no sampling).
using HistogramBins = std::array<std::array<uint32_t, 256>, 3>;

class Pipeline {
public:
    explicit Pipeline(std::shared_ptr<const ColorManager> colorManager);

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // Renders a preview whose longest side fits maxDim. When maxDim <= 0 the
    // full resolution is processed (export / 1:1). Results are cached.
    [[nodiscard]] ImageBufferPtr render(uint64_t imageId,
                                        const ImageBuffer& linear,
                                        const Adjustments& adjustments,
                                        int maxDim);
    [[nodiscard]] ImageBufferPtr renderFull(uint64_t imageId,
                                            const ImageBuffer& linear,
                                            const Adjustments& adjustments);
    // Renders at full resolution but stays in *linear ProPhoto* F32 space (no
    // sRGB conversion). Used for 16-bit TIFF and linear DNG export.
    [[nodiscard]] ImageBufferPtr renderLinearFull(
        uint64_t imageId, const ImageBuffer& linear,
        const Adjustments& adjustments);
    // Renders at full resolution to display-ready sRGB U8, but does NOT write
    // the result into the LRU cache (renderFull would). This is the 1:1 view
    // entry point: a full-size U8 buffer is far too large for the preview LRU
    // and would evict every thumbnail.
    [[nodiscard]] ImageBufferPtr renderFullDisplay(
        uint64_t imageId, const ImageBuffer& linear,
        const Adjustments& adjustments);

    // Renders to an F32 linear ProPhoto buffer at a bounded resolution: no sRGB
    // conversion and no LRU interaction. Soft proofing uses it to feed the
    // colourimetric proof transform from exactly the stage the display path
    // would convert (render() is renderInternalLinear() + toDisplaySRGB()), so a
    // proofed frame and an unproofed frame of the same edit differ only by the
    // proof transform. At coarse preview sizes it replaces that conversion
    // rather than adding a second full pass.
    //
    // @param linear  Linear ProPhoto source; never modified.
    // @param maxDim  Longest-edge cap; <= 0 keeps the source resolution.
    [[nodiscard]] ImageBufferPtr renderLinearBounded(
        const ImageBuffer& linear, const Adjustments& adjustments,
        int maxDim) const;

    // Preview-resolution render with per-stage incremental caching.
    // `source` is a linear ProPhoto buffer (F32 preferred; U16/U8 are converted);
    // it is downscaled (area-average, never upscaled) to fit `targetSize`.
    // `fullLongEdge` is the native full-resolution long edge used to scale the
    // absolute-pixel spatial kernels (texture/sharpening/NR/grain); 0 falls back
    // to the source's own long edge. `imageId` participates in the cache identity
    // (0 for callers without a catalog id). Returns display-ready sRGB U8.
    // The input is never modified.
    //
    // When `linearDisplayOut` is non-null and the render recomputes the stages
    // (i.e. not served entirely from the incremental cache), it receives the
    // adjusted linear ProPhoto F32 display buffer (the stage-F input) so callers
    // can derive a linear-light histogram without a second render pass. On a
    // full cache hit it is left untouched (nullptr); callers fall back to the
    // returned U8 buffer for the histogram.
    [[nodiscard]] ImageBufferPtr renderPreview(
        const ImageBuffer& source, const Adjustments& adjustments,
        const Size& targetSize, int fullLongEdge = 0, uint64_t imageId = 0,
        ImageBufferPtr* linearDisplayOut = nullptr);

    // Computes a display-encoded RGB histogram by traversing the ENTIRE buffer
    // (no sampling), so bin counts are exact.
    //
    // - F32 / U16 (linear working spaces): each channel value is sRGB-encoded
    //   (IEC 61966-2-1 OETF) and mapped to bin = floor(encoded * 256),
    //   clamped to 255. The encoding makes the histogram reflect the adjusted
    //   display luminance distribution rather than raw linear light.
    // - U8 (display-ready sRGB): the stored byte is the bin index directly,
    //   which keeps the fallback path bit-identical to the old preview-based
    //   histogram semantics.
    //
    // Returns an all-zero histogram for empty or non-3-channel buffers. Runs on
    // the calling thread; there is no internal state or lock. Typical cost for a
    // 1080p working buffer is a few ms (LUT-based encoding), so it must be
    // called off the UI thread (e.g. in the preview worker) to stay within the
    // per-frame budget.
    static HistogramBins computeHistogramFromLinear(const ImageBuffer& buffer);

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
    static float previewSigmaScale(int workingLongEdge,
                                   int fullLongEdge) noexcept;

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
    static SpatialKernels spatialKernels(const Adjustments& adjustments, int w,
                                         int h, float sigmaScale) noexcept;

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
    //
    // @warning The engine is stored as an atomic pointer to make concurrent
    //          reads well-defined, but callers MUST install it via
    //          setGlobalAiEngine() before any preview/export thread starts and
    //          must never replace it at runtime. setGlobalAiEngine() is meant
    //          to be called once from the main thread at startup.
    static AiInferenceEngine* globalAiEngine() noexcept;
    static void setGlobalAiEngine(AiInferenceEngine* engine) noexcept;

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
    static float maskWeight(const Mask& mask, int width, int height, float nx,
                            float ny) noexcept;

    // Rasterizes the mask's per-pixel weight into a w*h buffer (values in
    // [0,1]). Entry y*w+x is bit-identical to maskWeight at the same sample
    // point (nx = x/(w-1), ny = y/(h-1)); the mask's invert is folded in.
    // Results are cached globally, keyed by (Mask::geometryHash, w, h), so a
    // brush stroke / local-slider change that leaves the geometry intact hits
    // the cache. Returns null only for non-positive dimensions.
    [[nodiscard]] static std::shared_ptr<const std::vector<float>>
    brushWeightField(const Mask& mask, int width, int height);

    /**
     * @brief Rasterizes an AI mask's per-pixel weight against real pixel data.
     *
     * AI masks are content-driven, so their weight cannot come from
     * `brushWeightField` (which is geometry-only and deliberately returns a
     * neutral placeholder for them). This is the single entry point both the
     * render and the UI overlay use, so what the user sees is what gets applied.
     *
     * @param mask Mask with `type == MaskType::AiMask`; `mask.ai()` selects
     *        the target, threshold, feather and click prompts.
     * @param src Source pixels to segment (any ImageBuffer format: F32 / U16 /
     *        U8 RGB).
     * @return A width*height weight buffer matching `src`'s dimensions, or null
     *         when `mask` is not an AI mask or `src` is empty. Never null for a
     *         valid AI mask: AiMaskGenerator falls back to its pure-C++ heuristic
     *         when the model is unavailable, and to a zero mask beyond that.
     *
     * @note Runs the generator on the calling thread. The UI overlay must
     *       memoise the result (ImageViewport keeps a one-entry cache) so a
     *       repaint never re-runs inference.
     */
    [[nodiscard]] static std::shared_ptr<const std::vector<float>>
    aiMaskWeightField(const Mask& mask, const ImageBuffer& src);

    // Post-crop vignette weight field for the render buffer. Entry y*w+x is the
    // geometric vignette weight in [0,1] (0 at the buffer's geometric centre,
    // 1 at the corners). It depends only on midpoint/roundness/feather/style and
    // the buffer size -- NOT on amount/highlights -- so a pure amount change
    // reuses the cached field. Results are cached globally, keyed by those
    // parameters and w/h. Returns null only for non-positive
    // dimensions.
    [[nodiscard]] static std::shared_ptr<const std::vector<float>>
    postCropVignetteWeightField(const Adjustments& adjustments, int width,
                                int height);

    // Per-pixel weight [0,1] for a colour-range mask, compared in HSL space.
    // r/g/b are linear RGB [0,1]. Exposed for tests and the UI weight overlay.
    static float colorRangeWeight(const ColorRangeMask& mask, float r, float g,
                                  float b) noexcept;

    // --- Sub-mask weight helpers ---------------------------------------------
    // Per-pixel weight [0,1] of a geometry mask's colour-range modifier. The
    // distance is the max of normalized hue/sat/luma distances to the sampled
    // reference; `colorRange` sets the band width and `colorSmoothness` its
    // transition softness. Returns 1.0 (the identity element of the product)
    // when the modifier is disabled. r/g/b are linear ProPhoto RGB [0,1].
    static float colorSubMaskWeight(const Mask& mask, float r, float g,
                                    float b) noexcept;

    // Per-pixel weight [0,1] of a geometry mask's luminance-range modifier:
    // |L - luminanceRef| with L = 0.2126R + 0.7152G + 0.0722B. `luminanceRange`
    // sets the band width, `luminanceSmoothness` its softness. Returns 1.0
    // when the modifier is disabled.
    static float luminanceSubMaskWeight(const Mask& mask, float r, float g,
                                        float b) noexcept;

    // Combined final weight [0,1] at normalized image coordinates (nx, ny):
    // the geometry weight (invert folded in) combined with the enabled
    // colour/luminance sub-mask weights -- product for the default intersection
    // mode, max for union. With no sub-mask enabled this is exactly maskWeight,
    // so the pre-min/max combination behaviour is preserved. Exposed for tests
    // and the viewport's combined weight overlay.
    static float combinedMaskWeight(const Mask& mask, int width, int height,
                                    float nx, float ny, float r, float g,
                                    float b) noexcept;

    // Computes an automatic exposure/contrast/vibrance suggestion by analysing
    // the linear buffer's luminance (mean -> mid-gray, percentile stretch,
    // low saturation boost). Returns defaults when the image is empty.
    static Adjustments autoAdjust(const ImageBuffer& linear);

    // --- AI auto-develop suggestion (non-destructive, pure statistics) --
    /**
     * @brief A normalized crop rectangle (values in [0,1], left<right, top<bottom).
     *
     * It is a *suggestion* only: the pipeline currently has no crop-rendering
     * stage, so callers that apply an auto-develop result write exposure /
     * contrast / white-balance to the parameter stack and treat the crop box
     * as UI guidance. Kept separate from Adjustments on purpose so this
     * suggestion does not perturb the parameter-stack hash / serialization.
     */
    struct CropRect {
        float left = 0.0f;
        float top = 0.0f;
        float right = 1.0f;
        float bottom = 1.0f;
    };

    struct AutoDevelopSuggestion {
        double exposure = 0.0;      // stops
        double contrast = 0.0;      // -100..+100
        double temperature = 0.0;   // -100..+100
        double tint = 0.0;          // -100..+100
        CropRect crop;              // suggestion only (see CropRect)
        bool hasCrop = false;
    };

    /**
     * @brief One-shot auto-develop suggestion.
     *
     * Combines several lightweight statistics into a single parameter
     * recommendation: exposure (mean -> mid-gray), contrast (tonal span),
     * white-balance (grey-band channel means via estimateWhiteBalance), and a
     * conservative content-aware crop box. No new pixels are produced and the
     * input buffer is never mutated. `temperature`/`tint` use the same
     * -100..+100 slider domain as Adjustments.
     *
     * @param linear Linear ProPhoto buffer to analyse (may be downsampled by
     *               the caller to keep analysis cheap; F32 preferred).
     * @return The recommended fields; `hasCrop` is false when the content does
     *         not justify a crop (or the image is empty / degenerate).
     *
     * @note Deterministic; callable from any thread. Non-destructive contract.
     */
    static AutoDevelopSuggestion autoDevelop(const ImageBuffer& linear);


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
                                                     float lb) noexcept;

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
    [[nodiscard]] ImageBufferPtr visualizeOverlay(
        const ImageBuffer& linear, const Adjustments& adjustments,
        VisualizeMode mode, int maxDim) const;

private:
    struct CacheKey {
        uint64_t imageId;
        uint64_t adjHash;
        int maxDim;
        // Strided content fingerprint of the source buffer. Guards against a
        // different image (especially one reusing imageId==0) being served a
        // stale cached frame that happens to share imageId + adjHash + maxDim.
        uint64_t srcHash = 0;

        bool operator==(const CacheKey& o) const {
            return imageId == o.imageId && adjHash == o.adjHash &&
                   maxDim == o.maxDim && srcHash == o.srcHash;
        }
    };
    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const noexcept {
            size_t h = 1469598103934665603ull;
            h = (h ^ k.imageId) * 1099511628211ull;
            h = (h ^ k.adjHash) * 1099511628211ull;
            h = (h ^ static_cast<size_t>(k.maxDim)) * 1099511628211ull;
            h = (h ^ k.srcHash) * 1099511628211ull;
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
    //
    // Ownership model (this is what removes the per-stage deep copies):
    //  * `retained[0..3]` hold stage outputs as shared_ptr<const ImageBuffer>.
    //    The const type is the ownership guarantee: once a stage output is
    //    cached no writer exists, so later stages and later renders may read it
    //    directly instead of cloning it first.
    //  * A stage whose parameters are neutral is bit-identical to its input, so
    //    its output *is* the previous stage's snapshot: two retained slots may
    //    therefore alias one buffer. Every reader is const, so that is safe;
    //    the byte accounting counts each distinct allocation once.
    //  * Writable buffers only ever come from `WorkBufferPool`, which lives
    //    under the same mutex. A buffer is only pooled when it has no other
    //    owner, so a pooled (writable) buffer can never alias a retained
    //    (read-only) snapshot. Writes therefore cannot corrupt a cached stage.
    //
    // Stage buffers are exact-size F32 (width * height * 3 * 4 bytes).
    struct WorkBufferPool {
        // Idle F32 working buffers, all exactly width x height. Deliberately
        // small: the retained stages are the steady-state working set, and the
        // pool only exists to make a full recompute allocation-free.
        std::vector<ImageBufferPtr> idle;
        int width = 0;
        int height = 0;

        // Returns a writable F32 buffer of exactly w x h. Reuses an idle buffer
        // (no allocation, no zero-fill) or allocates one. A working-size change
        // drops the idle set, which only happens when the preview size changes.
        //
        // @note Caller must hold renderStateMutex_.
        ImageBufferPtr acquire(int w, int h);
        // Returns an exclusively owned buffer to the pool. A buffer with any
        // other owner (retained snapshot, caller-owned linear display buffer)
        // is dropped instead of pooled so it can never be written through a
        // live alias.
        //
        // @note Caller must hold renderStateMutex_; pass the only reference
        //       (std::move) so the no-other-owner check is meaningful.
        void release(ImageBufferPtr buffer);
        // Drops every idle buffer, releasing their memory.
        void clear();
    };
    struct RenderState {
        uint64_t sourceKey = 0;        // imageId + working size + content fingerprint
        uint64_t stageHashes[6] = {};  // last computed per-stage hashes
        // Immutable stage outputs A..D (index 4 / stage E is never retained).
        std::shared_ptr<const ImageBuffer> retained[4];
        bool present[4] = {};          // which stages are currently retained
        uint32_t accessTick[4] = {};   // LRU timestamp per retained stage
        uint32_t tick = 0;
        // Stage F (display-ready U8 sRGB). Mutable because renderPreview
        // returns it to callers, who may hold it across renders (existing
        // contract: a full cache hit hands back the same frame object).
        ImageBufferPtr finalFrame;
        bool valid = false;
        WorkBufferPool work;
    };
    void touchStageLocked(int stage);
    void evictIncrementalLocked();
    // Sum of the retained buffers' bytes, counting each distinct allocation
    // once (consecutive no-op stages share one immutable snapshot).
    //
    // @note Caller must hold renderStateMutex_.
    size_t retainedBytesLocked() const;
    // Releases retained[stage] (if present). `poolBuffer == true` recycles an
    // exclusively owned buffer into WorkBufferPool; `false` drops it so the
    // memory is actually given back (the eviction policy).
    //
    // @note Caller must hold renderStateMutex_.
    void dropRetainedStageLocked(int stage, bool poolBuffer);
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
    // Atomic so concurrent preview/export-thread reads are well-defined; see
    // the @warning on setGlobalAiEngine for the install-before-threads rule.
    static std::atomic<AiInferenceEngine*> aiEngine_;
};

} // namespace lrc
