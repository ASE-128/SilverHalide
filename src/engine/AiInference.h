#pragma once

// ---------------------------------------------------------------------------
// AiInferenceEngine — pure C++ ONNX Runtime wrapper.
//
// DESIGN RULES (enforced by this file):
//   • NO Qt includes anywhere in this header or the .cpp.
//   • All public types and signatures are identical whether LRC_ENABLE_AI=1
//     (real ONNX Runtime) or LRC_ENABLE_AI=0 (compiled-in stubs).
//   • When LRC_ENABLE_AI=0 every method returns a well-defined error value
//     (false / empty vector / null buffer) so callers never need #ifdef.
//   • A special "builtin://<name>" model path activates pure-C++ reference
//     implementations used for CI unit tests. These bypass ONNX entirely and
//     keep the test suite green even when no real models are downloaded.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "engine/ImageBuffer.h"

namespace lrc {

struct ModelShape {
    std::vector<int64_t> dims;       // e.g. {1, 3, 224, 224}; -1 means dynamic
    std::string layout = "NCHW";     // "NCHW" or "NHWC" for image tensors
};

struct ModelInfo {
    std::string id;                  // e.g. "nafnet-denoise"
    std::filesystem::path onnxPath;  // absolute path to the .onnx file
    ModelShape inputShape;
    ModelShape outputShape;
    // Optional normalization applied before inference:
    //   input[n] = (pixel[n] - mean[c]) / std[c]   (NCHW, c in 0..2)
    std::vector<float> inputMean = {0.0f, 0.0f, 0.0f};
    std::vector<float> inputStd  = {1.0f, 1.0f, 1.0f};
};

/**
 * @brief Unified inference engine. One instance per process is sufficient:
 *        sessions are keyed by (path, thread-id) so Session::Run is never
 *        called concurrently on the same Ort::Session.
 *
 * Typical usage:
 *   AiInferenceEngine engine;
 *   ModelInfo info{"nafnet", path, {1,3,512,512}, {1,3,512,512}};
 *   if (engine.loadModel(info)) {
 *       auto out = engine.run(inputFloats, {1,3,H,W});
 *       ...
 *   }
 */
class AiInferenceEngine {
public:
    AiInferenceEngine();
    ~AiInferenceEngine();

    AiInferenceEngine(const AiInferenceEngine&) = delete;
    AiInferenceEngine& operator=(const AiInferenceEngine&) = delete;

    /**
     * @brief Load / register a model. For real ONNX models this pre-warms a
     *        thread-local session on the calling thread (so the first run()
     *        on that thread avoids lazy-init cost). Returns true on success.
     *        Safe to call multiple times with the same id — subsequent calls
     *        are no-ops and return true.
     */
    bool loadModel(const ModelInfo& info);

    /** Convenience overload — loads a path-only model with empty shape hints. */
    bool loadModel(const std::filesystem::path& modelPath);

    /**
     * @brief Run raw-tensor inference on the model previously registered as
     *        `modelId`. Input must already be in the layout the model expects
     *        (use preprocessImage() to convert an ImageBuffer first).
     *        Returns an empty vector on failure.
     */
    std::vector<float> run(const std::string& modelId,
                           const std::vector<float>& input,
                           const std::vector<int64_t>& inputShape);

    /**
     * @brief Full-image helper for AI pipelines: preprocesses an ImageBuffer
     *        (format conversion -> resize -> NCHW -> mean/std) -> runs() ->
     *        post-processes back into an ImageBuffer of the requested size.
     *        If outputShape has 4 dims they are interpreted as {1,C,H,W};
     *        if H/W == -1 the input H/W is preserved.
     */
    ImageBufferPtr runImage(const std::string& modelId,
                            const ImageBuffer& input,
                            const ModelShape& outputShape);

    /**
     * @brief Image pre-processing pipeline (exposed so callers can inspect
     *        the tensor before run, and for unit tests).
     *        • Converts buffer to F32 RGB [0,1] interleaved
     *        • Resizes to targetH x targetW (bilinear-like pixel averaging)
     *        • NHWC -> NCHW transpose
     *        • Applies per-channel (x - mean) / std
     */
    static std::vector<float> preprocessImage(
        const ImageBuffer& input,
        int targetW, int targetH,
        const std::vector<float>& mean = {0.0f, 0.0f, 0.0f},
        const std::vector<float>& stdv = {1.0f, 1.0f, 1.0f});

    /**
     * @brief Inverse of preprocessImage for outputs that are an image tensor.
     *        Converts NCHW float tensor (with optional mean/std reversion) to
     *        an F32 or U8 ImageBuffer of the given dimensions.
     */
    static ImageBufferPtr postprocessImage(
        const std::vector<float>& tensor,
        int w, int h, int channels,
        PixelFormat outFormat = PixelFormat::F32,
        const std::vector<float>& mean = {0.0f, 0.0f, 0.0f},
        const std::vector<float>& stdv = {1.0f, 1.0f, 1.0f});

    /** Returns true when a model with the given id has been loaded. */
    bool hasModel(const std::string& modelId) const;

    /** Returns the registered info for modelId (or nullopt when absent). */
    std::optional<ModelInfo> modelInfo(const std::string& modelId) const;

    /** True when the engine was built with real ONNX Runtime support. */
    static constexpr bool hasRealBackend() {
#if LRC_ENABLE_AI
        return true;
#else
        return false;
#endif
    }

    /**
     * @brief Preferred ONNX execution provider.
     *
     * DirectML (GPU) is used only when it is actually available at runtime;
     * otherwise the engine silently falls back to the CPU provider. Pure CPU
     * workloads are unaffected and the builtin (unit-test) models never touch
     * ONNX at all. Default is Cpu.
     */
    enum class ExecutionProvider { Cpu, DirectML };

    /** Sets the preferred provider (applies to sessions created afterwards). */
    void setPreferredProvider(ExecutionProvider provider);

    /** Returns the currently preferred provider. */
    ExecutionProvider preferredProvider() const;

    /**
     * @brief Whether the given provider is present in this ONNX Runtime build.
     *
     * DirectML returns false when LRC_ENABLE_AI=0 or the runtime lacks a
     * DirectML component. The query never throws.
     */
    static bool isProviderAvailable(ExecutionProvider provider);

    /** Human-readable last-error message (empty if the last call succeeded). */
    std::string lastError() const;

private:
    // Per-thread session handle. We use an opaque pointer so the ONNX headers
    // are only pulled into the .cpp TU. Each thread gets its own Ort::Session
    // because single-session Run() is not thread-safe (per ONNX docs).
    struct SessionHolder;
    struct Impl;
    std::unique_ptr<Impl> impl_;

    mutable std::shared_mutex mutex_;
    // Dedicated mutex for the error state. Reads/writes to `lastError_`
    // are guarded by this mutex instead of `mutex_`, because run()/runImage()
    // hold a *shared* lock on `mutex_` for the whole inference body and must
    // never try to upgrade to a write on it (that would self-deadlock).
    mutable std::shared_mutex errMutex_;
    mutable std::string lastError_;
};

} // namespace lrc
