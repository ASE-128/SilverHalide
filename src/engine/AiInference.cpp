// AiInferenceEngine implementation.
//
// Compile-time branches:
//   • LRC_ENABLE_AI=1  -> pull in <onnxruntime_cxx_api.h>, real sessions.
//   • LRC_ENABLE_AI=0  -> stubs + builtin models only.
//
// In both modes the BUILTIN models are always available (pure-C++ reference
// implementations used by CI).  Currently:
//     "builtin://add_2x2"    — element-wise add of a 1x2x2 tensor with a
//                              constant; used by TestAiInference.cpp.

#include "engine/AiInference.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <sstream>
#include <thread>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>  // GetProcAddress for dynamically loading the DML append fn
#endif

#if LRC_ENABLE_AI
// Real ONNX Runtime C++ API.  Pulled in only when the engine target has
// ONNX Runtime linked (otherwise this TU can't compile).
#include <onnxruntime_cxx_api.h>
#endif

namespace lrc {

// ---------------------------------------------------------------------------
// Tiny helpers (always available)
// ---------------------------------------------------------------------------
namespace {

// Thread-safe error state accessor for AiInferenceEngine.
// All reads and writes to `lastError_` go through these helpers, which take the
// DEDICATED error mutex `errMutex_` (never `mutex_`): run()/runImage() hold a
// shared lock on `mutex_` for their whole body, so writing `lastError_` under
// `mutex_` from inside them would self-deadlock. Using `errMutex_` both fixes
// the original data race and avoids that deadlock.
inline std::string getLastError(std::string& err,
                                std::shared_mutex& mx) {
    // `mx` must be non-const: std::shared_lock<std::shared_mutex> can only
    // lock a mutable mutex (unlock_shared() is non-const). The mutex is held
    // by member `errMutex_` here.
    std::shared_lock lock(mx);
    return err;
}
inline void setLastError(std::string& err, const std::string& msg,
                         std::shared_mutex& mx) {
    std::unique_lock lock(mx);
    err = msg;
}
inline void clearLastError(std::string& err, std::shared_mutex& mx) {
    std::unique_lock lock(mx);
    err.clear();
}

// Bilinear-ish resize for float RGB buffers (simple area average for downscaling,
// nearest otherwise).  Kept header-free; matches ImageBuffer::resize semantics
// but works on raw float vectors so we don't need a round-trip.
std::vector<float> resizeFloatRGB(const float* src, int sw, int sh,
                                  int dw, int dh) {
    std::vector<float> out(static_cast<size_t>(dw) * static_cast<size_t>(dh) * 3);
    if (sw == dw && sh == dh) {
        std::copy(src, src + static_cast<size_t>(sw) * sh * 3, out.begin());
        return out;
    }
    // Nearest-neighbour fallback (sufficient for AI preprocessing which
    // typically runs after an ImageBuffer::resize that already produced
    // close-to-target dimensions).
    for (int y = 0; y < dh; ++y) {
        const int sy = std::min(sh - 1, static_cast<int>(static_cast<float>(y) *
                                 static_cast<float>(sh) / static_cast<float>(dh)));
        for (int x = 0; x < dw; ++x) {
            const int sx = std::min(sw - 1, static_cast<int>(static_cast<float>(x) *
                                     static_cast<float>(sw) / static_cast<float>(dw)));
            const size_t si = (static_cast<size_t>(sy) * sw + sx) * 3;
            const size_t di = (static_cast<size_t>(y) * dw + x) * 3;
            out[di]     = src[si];
            out[di + 1] = src[si + 1];
            out[di + 2] = src[si + 2];
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Builtin reference models (pure C++, always-on)
// ---------------------------------------------------------------------------
enum class BuiltinKind { None, Add2x2 };

BuiltinKind builtinKindForPath(const std::filesystem::path& p) {
    const auto s = p.string();
    if (s == "builtin://add_2x2") return BuiltinKind::Add2x2;
    return BuiltinKind::None;
}

std::vector<float> runBuiltin(BuiltinKind kind,
                              const std::vector<float>& in,
                              const std::vector<int64_t>& shape) {
    switch (kind) {
        case BuiltinKind::Add2x2: {
            // Adds a constant 0.5 to every element of a 1x2x2 (or smaller)
            // tensor.  Used by the unit test to prove load→run→output shape.
            std::vector<float> out = in;
            for (auto& v : out) v += 0.5f;
            return out;
        }
        default:
            return {};
    }
}

// ---------------------------------------------------------------------------
// Execution-provider helpers (DirectML)
// ---------------------------------------------------------------------------

// True when this ONNX Runtime build exposes a DirectML execution provider.
// Returns false for a stub build (LRC_ENABLE_AI=0) or a CPU-only runtime. The
// availability query is wrapped so it can never throw.
#if LRC_ENABLE_AI
inline bool directMLAvailable() {
    try {
        // ONNX Runtime reports the DirectML EP as "DmlExecutionProvider"
        // (mixed case). Compare case-insensitively so detection is robust to
        // upstream casing changes. Without this, a DirectML-capable runtime
        // would be silently reported unavailable and fall back to CPU.
        const auto providers = Ort::GetAvailableProviders();
        for (const auto& p : providers) {
            if (p.size() != 18) continue;               // len("DmlExecutionProvider")
            bool same = true;
            for (size_t i = 0; i < 18; ++i)
                if (static_cast<char>(std::tolower((unsigned char)p[i])) !=
                    "dmlexecutionprovider"[i]) { same = false; break; }
            if (same) return true;
        }
    } catch (const Ort::Exception&) {
        return false;
    }
    return false;
}

// Appends the DirectML EP to `opts` when it is present. The append symbol is
// resolved at runtime (GetProcAddress) rather than linked directly, so this TU
// builds and runs against ONNX Runtime builds that ship without a DirectML
// component (e.g. the vcpkg CPU-only port) as long as DirectML is absent —
// when it is present we call through the resolved function.
inline void appendDirectMLIfAvailable(Ort::SessionOptions& opts) {
    if (!directMLAvailable())
        return;
    using AppendFn = OrtStatus*(ORT_API_CALL*)(OrtSessionOptions*);
    auto module = ::GetModuleHandleW(L"onnxruntime.dll");
    if (!module)
        module = ::GetModuleHandleW(L"onnxruntime");
    if (!module)
        return;
    auto fn = reinterpret_cast<AppendFn>(
        reinterpret_cast<void*>(::GetProcAddress(module,
                                                 "OrtSessionOptionsAppendExecutionProvider_DML")));
    if (!fn)
        return;
    Ort::ThrowOnError(fn(opts));  // sets lastError in the caller's catch loop
}

// Assembles the shared session options. `useDirectML` requests the DirectML EP
// but only when it is actually present — otherwise everything falls back to CPU.
inline Ort::SessionOptions makeSessionOptions(bool useDirectML) {
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
    if (useDirectML)
        appendDirectMLIfAvailable(opts);
    return opts;
}
#endif

} // namespace

// ---------------------------------------------------------------------------
// AiInferenceEngine opaque impl (SessionHolder + model registry)
// ---------------------------------------------------------------------------
struct AiInferenceEngine::SessionHolder {
#if LRC_ENABLE_AI
    std::unique_ptr<Ort::Session> session;
#endif
    BuiltinKind builtin = BuiltinKind::None;
};

struct AiInferenceEngine::Impl {
    // Model id -> registered ModelInfo (copy so the registry is self-contained)
    std::unordered_map<std::string, ModelInfo> models;

    // Preferred execution provider (DirectML when requested and available).
    AiInferenceEngine::ExecutionProvider preferred =
        AiInferenceEngine::ExecutionProvider::Cpu;

    // Thread-id -> (modelId -> SessionHolder).  Each thread owns its own
    // Ort::Session because a single session isn't safe for concurrent Run.
    using PerModelSessions = std::unordered_map<std::string, SessionHolder>;
    using ThreadSessions = std::unordered_map<std::thread::id, PerModelSessions>;
    ThreadSessions sessions;
    std::mutex sessionsMutex;  // guards `sessions` (not per-session Run calls)

#if LRC_ENABLE_AI
    // The ONNX Runtime env is a process-wide singleton; Ort::Env isn't
    // trivially copyable so we create it once inside Impl and share it via
    // raw pointer into every Session (sessions hold a non-owning view of the
    // env anyway — only the memory-allocation callbacks are shared, and
    // those are thread-safe).
    std::unique_ptr<Ort::Env> env;
    Ort::AllocatorWithDefaultOptions allocator;

    Impl() {
        try {
            env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,
                                              "SilverHalide_Ai");
        } catch (const Ort::Exception&) {
            env.reset();  // leave in a stub-compatible state
        }
    }
#endif
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
AiInferenceEngine::AiInferenceEngine() : impl_(std::make_unique<Impl>()) {}
AiInferenceEngine::~AiInferenceEngine() = default;

// ---------------------------------------------------------------------------
// loadModel
// ---------------------------------------------------------------------------
bool AiInferenceEngine::loadModel(const ModelInfo& info) {
    clearLastError(lastError_, errMutex_);
    if (info.id.empty()) {
        setLastError(lastError_, "loadModel: empty id", errMutex_);
        return false;
    }

    // Decide if this is a builtin, real-ONNX file, or nothing loadable.
    // We only upsert into `models_` after the load succeeds so callers can
    // rely on !hasModel(x) after a failed loadModel(x).
    const BuiltinKind bk = builtinKindForPath(info.onnxPath);

    if (bk != BuiltinKind::None) {
        // --- Builtin path: register only, nothing external to load ---------
        std::unique_lock lock(mutex_);
        impl_->models[info.id] = info;
        std::unique_lock<std::mutex> sl(impl_->sessionsMutex);
        SessionHolder h;
        h.builtin = bk;
        impl_->sessions[std::this_thread::get_id()][info.id] = std::move(h);
        return true;
    }

    // --- Real-ONNX path: pre-warm a session for the calling thread ----------
#if LRC_ENABLE_AI
    if (!impl_->env) {
        setLastError(lastError_, "ONNX Runtime env not initialised", errMutex_);
        return false;
    }
    if (!std::filesystem::exists(info.onnxPath)) {
        std::ostringstream oss;
        oss << "ONNX file not found: " << info.onnxPath;
        setLastError(lastError_, oss.str(), errMutex_);
        return false;
    }
    try {
        Ort::SessionOptions opts = makeSessionOptions(
            impl_->preferred == ExecutionProvider::DirectML);

        const auto wide = info.onnxPath.wstring();
        auto session = std::make_unique<Ort::Session>(
            *impl_->env, wide.c_str(), opts);

        SessionHolder h;
        h.session = std::move(session);
        // Now that everything external succeeded, commit to the registry.
        {
            std::unique_lock lock(mutex_);
            impl_->models[info.id] = info;
        }
        std::unique_lock<std::mutex> sl(impl_->sessionsMutex);
        impl_->sessions[std::this_thread::get_id()][info.id] = std::move(h);
        return true;
    } catch (const Ort::Exception& e) {
        setLastError(lastError_, std::string("ONNX load error: ") + e.what(), errMutex_);
        return false;
    }
#else
    setLastError(lastError_,
             "LRC_ENABLE_AI=0: real ONNX models unavailable. "
             "Rebuild with ONNX Runtime installed to use " + info.id, errMutex_);
    return false;
#endif
}

bool AiInferenceEngine::loadModel(const std::filesystem::path& modelPath) {
    ModelInfo info;
    info.id         = modelPath.string();  // use the path itself as the id
    info.onnxPath   = modelPath;
    return loadModel(info);
}

void AiInferenceEngine::setPreferredProvider(ExecutionProvider provider) {
    std::unique_lock lock(mutex_);
    impl_->preferred = provider;
}

AiInferenceEngine::ExecutionProvider AiInferenceEngine::preferredProvider() const {
    std::shared_lock lock(mutex_);
    return impl_->preferred;
}

bool AiInferenceEngine::isProviderAvailable(ExecutionProvider provider) {
    if (provider == ExecutionProvider::Cpu)
        return true;
#if LRC_ENABLE_AI
    return directMLAvailable();
#else
    (void)provider;
    return false;
#endif
}

bool AiInferenceEngine::hasModel(const std::string& modelId) const {
    std::shared_lock lock(mutex_);
    return impl_->models.find(modelId) != impl_->models.end();
}

AiInferenceEngine::SessionHolder* AiInferenceEngine::getOrCreateSession(
    const std::string& modelId) {
    // Look up model info first (guards against unloaded ids). We copy the info
    // before dropping the read lock so we don't hold `mutex_` while creating a
    // session (session load is slow and run()/runImage() want the shared lock
    // free for concurrent readers).
    std::optional<ModelInfo> infoOpt;
    {
        std::shared_lock rlock(mutex_);
        auto it = impl_->models.find(modelId);
        if (it == impl_->models.end()) {
            setLastError(lastError_, "run: model not loaded: " + modelId, errMutex_);
            return nullptr;
        }
        infoOpt = it->second;
    }
    const ModelInfo& info = *infoOpt;

    // Get-or-create a session for this thread.
    const auto tid = std::this_thread::get_id();
    std::unique_lock<std::mutex> sl(impl_->sessionsMutex);
    auto& perModel = impl_->sessions[tid];
    auto hIt = perModel.find(modelId);
    if (hIt != perModel.end())
        return &hIt->second;

    // First call on this thread — lazy-init.  For builtin models this is
    // cheap; for real ONNX models this means the first `run` pays the
    // session-load cost (a warn-worthy but functional path).
    const BuiltinKind bk = builtinKindForPath(info.onnxPath);
    if (bk != BuiltinKind::None) {
        SessionHolder h;
        h.builtin = bk;
        auto res = perModel.emplace(modelId, std::move(h));
        return &res.first->second;
    }
#if LRC_ENABLE_AI
    if (!impl_->env || !std::filesystem::exists(info.onnxPath)) {
        setLastError(lastError_, "run: lazy session init failed for " + modelId, errMutex_);
        return nullptr;
    }
    try {
        Ort::SessionOptions opts = makeSessionOptions(
            impl_->preferred == ExecutionProvider::DirectML);
        const auto wide = info.onnxPath.wstring();
        auto s = std::make_unique<Ort::Session>(*impl_->env, wide.c_str(), opts);
        SessionHolder h;
        h.session = std::move(s);
        auto res = perModel.emplace(modelId, std::move(h));
        return &res.first->second;
    } catch (const Ort::Exception& e) {
        setLastError(lastError_, std::string("ONNX lazy-load: ") + e.what(), errMutex_);
        return nullptr;
    }
#else
    setLastError(lastError_, "run: LRC_ENABLE_AI=0, cannot run " + modelId, errMutex_);
    return nullptr;
#endif
}

std::optional<ModelInfo> AiInferenceEngine::modelInfo(const std::string& modelId) const {
    std::shared_lock lock(mutex_);
    auto it = impl_->models.find(modelId);
    if (it == impl_->models.end()) return std::nullopt;
    return it->second;
}

std::string AiInferenceEngine::lastError() const {
    return getLastError(lastError_, errMutex_);
}

// ---------------------------------------------------------------------------
// run() — raw tensor
// ---------------------------------------------------------------------------
std::vector<float> AiInferenceEngine::run(const std::string& modelId,
                                          const std::vector<float>& input,
                                          const std::vector<int64_t>& inputShape) {
    clearLastError(lastError_, errMutex_);

    // 1. Look up model info ---------------------------------------------------
    std::shared_lock rlock(mutex_);
    auto infoIt = impl_->models.find(modelId);
    if (infoIt == impl_->models.end()) {
        setLastError(lastError_, "run: model not loaded: " + modelId, errMutex_);
        return {};
    }
    const ModelInfo info = infoIt->second;
    rlock.unlock();

    // 2. Get-or-create a session for this thread -----------------------------
    SessionHolder* holder = getOrCreateSession(modelId);
    if (!holder)
        return {};

    // 3. Builtin fast-path ----------------------------------------------------
    if (holder->builtin != BuiltinKind::None) {
        return runBuiltin(holder->builtin, input, inputShape);
    }

    // 4. Real ONNX Run --------------------------------------------------------
#if LRC_ENABLE_AI
    try {
        Ort::Session& sess = *holder->session;

        // Get input/output names via the allocator
        Ort::AllocatorWithDefaultOptions alloc;
        auto inputNamesRaw = sess.GetInputNameAllocated(0, alloc);
        const char* inputNames[] = {inputNamesRaw.get()};
        auto outputNamesRaw = sess.GetOutputNameAllocated(0, alloc);
        const char* outputNames[] = {outputNamesRaw.get()};

        auto memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        std::vector<int64_t> inShape = inputShape;
        size_t expectedElems = 1;
        for (auto d : inShape) expectedElems *= static_cast<size_t>(d);
        if (input.size() != expectedElems) {
            std::ostringstream oss;
            oss << "input size mismatch: expected " << expectedElems
                << " got " << input.size();
            setLastError(lastError_, oss.str(), errMutex_);
            return {};
        }

        auto inputTensor = Ort::Value::CreateTensor<float>(
            memoryInfo,
            const_cast<float*>(input.data()), input.size(),
            inShape.data(), inShape.size());

        auto outputTensors = sess.Run(
            Ort::RunOptions{nullptr},
            inputNames, &inputTensor, 1,
            outputNames, 1);

        if (outputTensors.empty()) {
            setLastError(lastError_, "ONNX run: no outputs", errMutex_);
            return {};
        }

        Ort::Value& outVal = outputTensors.front();
        if (!outVal.IsTensor()) {
            setLastError(lastError_, "ONNX run: output is not a tensor", errMutex_);
            return {};
        }
        auto outShapeInfo = outVal.GetTensorTypeAndShapeInfo();
        const size_t n = outShapeInfo.GetElementCount();
        const float* outRaw = outVal.GetTensorData<float>();
        return std::vector<float>(outRaw, outRaw + n);
    } catch (const Ort::Exception& e) {
        setLastError(lastError_, std::string("ONNX run: ") + e.what(), errMutex_);
        return {};
    }
#else
    setLastError(lastError_, "run: LRC_ENABLE_AI=0 — stubs cannot run real models", errMutex_);
    return {};
#endif
}

// ---------------------------------------------------------------------------
// runImage() — convenience helper
// ---------------------------------------------------------------------------
ImageBufferPtr AiInferenceEngine::runImage(const std::string& modelId,
                                           const ImageBuffer& input,
                                           const ModelShape& outputShape) {
    // 1. Resolve model info + target input dims --------------------------------
    std::shared_lock rlock(mutex_);
    auto infoIt = impl_->models.find(modelId);
    if (infoIt == impl_->models.end()) {
        setLastError(lastError_, "runImage: model not loaded: " + modelId, errMutex_);
        return nullptr;
    }
    ModelInfo info = infoIt->second;
    rlock.unlock();

    // Dynamic dims fall back to the input's H x W
    const int origW = input.width();
    const int origH = input.height();
    const auto& inDims = info.inputShape.dims;
    int iw = origW, ih = origH;
    // Expected convention: {N, C, H, W} for NCHW, or {N, H, W, C} for NHWC
    if (inDims.size() >= 4) {
        if (info.inputShape.layout == "NCHW") {
            if (inDims[2] > 0) ih = static_cast<int>(inDims[2]);
            if (inDims[3] > 0) iw = static_cast<int>(inDims[3]);
        } else { // NHWC
            if (inDims[1] > 0) ih = static_cast<int>(inDims[1]);
            if (inDims[2] > 0) iw = static_cast<int>(inDims[2]);
        }
    }
    if (iw <= 0) iw = origW;
    if (ih <= 0) ih = origH;

    // 2. Preprocess -----------------------------------------------------------
    std::vector<float> tensor = preprocessImage(input, iw, ih,
                                                info.inputMean, info.inputStd);

    std::vector<int64_t> inShape;
    if (info.inputShape.layout == "NCHW") {
        inShape = {1, 3, ih, iw};
    } else {
        inShape = {1, ih, iw, 3};
    }

    // 3. Run ------------------------------------------------------------------
    std::vector<float> out = run(modelId, tensor, inShape);
    if (out.empty()) return nullptr;

    // 4. Decide output dims (from outputShape hint or match input dims) -------
    int ow = origW, oh = origH, oc = 3;
    const auto& od = outputShape.dims;
    if (!od.empty()) {
        // Handle NCHW / NHWC convention by length
        if (od.size() == 2) { oc = static_cast<int>(od[1]); oh = 1; ow = 1; }
        else if (od.size() == 4) {
            if (outputShape.layout == "NCHW") {
                oc = (od[1] > 0) ? static_cast<int>(od[1]) : 3;
                oh = (od[2] > 0) ? static_cast<int>(od[2]) : origH;
                ow = (od[3] > 0) ? static_cast<int>(od[3]) : origW;
            } else {
                oh = (od[1] > 0) ? static_cast<int>(od[1]) : origH;
                ow = (od[2] > 0) ? static_cast<int>(od[2]) : origW;
                oc = (od[3] > 0) ? static_cast<int>(od[3]) : 3;
            }
        }
    }
    // Safety clamp for wildly mismatched tensors
    const size_t needed = static_cast<size_t>(ow) * oh * oc;
    if (out.size() < needed) {
        // Not enough output data — re-derive dimensions from the flat size.
        const size_t perPx = std::max<size_t>(1, oc);
        const size_t px = out.size() / perPx;
        ow = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(px))));
        oh = std::max(1, static_cast<int>(px / ow));
    }

    // 5. Post-process back to an image buffer ---------------------------------
    // For segmentation-like 1-channel outputs we replicate to 3 channels only
    // when the caller asked for a 3-channel buffer; otherwise keep 1-channel
    // semantics by packing mask pixels as RRR (the mask consumers read R).
    auto buf = postprocessImage(out, ow, oh, oc, PixelFormat::F32,
                                info.inputMean, info.inputStd);
    return buf;
}

// ---------------------------------------------------------------------------
// runSamPoint — two-stage MobileSAM click-prompt segmentation
// ---------------------------------------------------------------------------
namespace {

// Sigmoid activation for SAM mask logits -> [0,1] weight. MobileSAM emits raw
// mask logits; the renderer consumes a weight field in [0,1], so we map the
// best candidate through sigmoid (consistent with SAM's sigmoid/0.5 threshold).
inline float sigmoid(float x) {
    if (x >= 0.0f) { const float z = std::exp(-x); return 1.0f / (1.0f + z); }
    const float z = std::exp(x); return z / (1.0f + z);
}

// Nearest-neighbour 1-channel resize (Hsrc x Wsrc -> Hdst x Wdst, row-major).
inline std::vector<float> resize1Ch(const std::vector<float>& src,
                                    int sw, int sh, int dw, int dh) {
    std::vector<float> out(static_cast<size_t>(dw) * dh);
    for (int y = 0; y < dh; ++y) {
        const int sy = std::min(sh - 1, static_cast<int>(static_cast<float>(y) *
                                 static_cast<float>(sh) / static_cast<float>(dh)));
        for (int x = 0; x < dw; ++x) {
            const int sx = std::min(sw - 1, static_cast<int>(static_cast<float>(x) *
                                     static_cast<float>(sw) / static_cast<float>(dw)));
            out[static_cast<size_t>(y) * dw + x] = src[static_cast<size_t>(sy) * sw + sx];
        }
    }
    return out;
}

} // namespace

SamMaskResult AiInferenceEngine::runSamPoint(const std::string& encoderId,
                                             const std::string& decoderId,
                                             const ImageBuffer& input,
                                             const std::vector<SamPoint>& points) {
    SamMaskResult result;
    clearLastError(lastError_, errMutex_);

    const int origW = input.width(), origH = input.height();
    if (origW <= 0 || origH <= 0 || points.empty()) {
        setLastError(lastError_, "runSamPoint: bad input image or empty points", errMutex_);
        return result;  // ok=false
    }
    if (!hasModel(encoderId) || !hasModel(decoderId)) {
        setLastError(lastError_, "runSamPoint: encoder/decoder not loaded", errMutex_);
        return result;
    }

    // --- 1. Encode the image (longest edge 1024, pad-to-square, NHWC) ---------
    // MobileSAM input space: image resized so the longest edge is 1024, then
    // zero-padded on the right/bottom to a square 1024x1024. Points map into
    // this space by the same scale factor (padding only extends the canvas, so
    // interior coordinates are unshifted). Pixel values are normalized with the
    // 255-scaled ImageNet stats, in NHWC [1024,1024,3] matching the real
    // exported encoder input_image [H,W,3] (dynamic NHWC).
    constexpr int IMG = 1024;
    const float scale = static_cast<float>(IMG) / static_cast<float>(std::max(origW, origH));
    const int newW = std::max(1, static_cast<int>(static_cast<float>(origW) * scale + 0.5f));
    const int newH = std::max(1, static_cast<int>(static_cast<float>(origH) * scale + 0.5f));

    // float RGB [0,255] of original image (any input format -> F32-like copy).
    std::vector<float> src(static_cast<size_t>(origW) * origH * 3);
    {
        const auto& raw = input.data();
        if (input.format() == PixelFormat::U8) {
            for (size_t i = 0; i < src.size(); ++i)
                src[i] = static_cast<float>(raw[i]);
        } else if (input.format() == PixelFormat::U16) {
            const auto* r16 = reinterpret_cast<const uint16_t*>(raw.data());
            for (size_t i = 0; i < src.size(); ++i)
                src[i] = static_cast<float>(r16[i]) * (255.0f / 65535.0f);
        } else {
            const auto* rf = reinterpret_cast<const float*>(raw.data());
            for (size_t i = 0; i < src.size(); ++i)
                src[i] = rf[i] * 255.0f;
        }
    }
    std::vector<float> resized = resizeFloatRGB(src.data(), origW, origH, newW, newH);

    // To NHWC [1024,1024,3]: zero-pad the right/bottom and normalize with the
    // 255-scaled ImageNet stats, interleaved (row-major HxW, RGB per pixel) to
    // match the fixed [1024,1024,3] tensor fed to the encoder.
    constexpr float kM0 = 123.675f, kM1 = 116.28f,  kM2 = 103.53f;
    constexpr float kS0 = 58.395f,  kS1 = 57.12f,   kS2 = 57.375f;
    std::vector<float> img(static_cast<size_t>(IMG) * IMG * 3);
    for (size_t i = 0; i < img.size(); i += 3) {
        img[i] = -kM0 / kS0;  // pre-fill from the zero-pad value
        img[i + 1] = -kM1 / kS1;
        img[i + 2] = -kM2 / kS2;
    }
    for (int y = 0; y < newH; ++y) {
        for (int x = 0; x < newW; ++x) {
            const size_t si = (static_cast<size_t>(y) * newW + x) * 3;
            const size_t di = (static_cast<size_t>(y) * IMG + x) * 3;
            img[di]     = (resized[si]     - kM0) / kS0;
            img[di + 1] = (resized[si + 1] - kM1) / kS1;
            img[di + 2] = (resized[si + 2] - kM2) / kS2;
        }
    }

    std::vector<float> embedding = run(encoderId, img, {IMG, IMG, 3});
    constexpr size_t EMB = 256ull * 64u * 64u;
    if (embedding.size() != EMB) {
        setLastError(lastError_, "runSamPoint: encoder output unexpected size", errMutex_);
        return result;
    }

    // --- 2. Decode with the point prompts -------------------------------------
    SessionHolder* holder = getOrCreateSession(decoderId);
    if (!holder) {
        return result;  // lastError already set
    }
    if (holder->builtin != BuiltinKind::None) {
        setLastError(lastError_, "runSamPoint: builtin decoder not supported", errMutex_);
        return result;
    }
#if LRC_ENABLE_AI
    try {
        Ort::Session& sess = *holder->session;
        Ort::AllocatorWithDefaultOptions alloc;
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,
                                                         OrtMemTypeDefault);

        // Point tensors in the 1024-padded coordinate space.
        const int P = static_cast<int>(points.size());
        std::vector<float> coords(static_cast<size_t>(P) * 2);
        std::vector<float> labels(static_cast<size_t>(P));
        for (int p = 0; p < P; ++p) {
            coords[static_cast<size_t>(p) * 2]     = points[p].nx * static_cast<float>(newW);
            coords[static_cast<size_t>(p) * 2 + 1] = points[p].ny * static_cast<float>(newH);
            labels[static_cast<size_t>(p)] = static_cast<float>(points[p].label);
        }
        const std::vector<int64_t> coordShape = {1, P, 2};
        const std::vector<int64_t> labelShape = {1, P};
        const std::vector<int64_t> embShape   = {1, 256, 64, 64};
        const std::vector<int64_t> maskShape  = {1, 1, 256, 256};
        const std::vector<int64_t> hasShape   = {1};
        const std::vector<int64_t> origShape  = {2};
        const std::vector<float> maskInput(256u * 256u, 0.0f);
        const std::vector<float> hasMask = {0.0f};
        const std::vector<float> origSz  = {static_cast<float>(newH),
                                            static_cast<float>(newW)};

        // Name every input/output slot (decoder has 6 inputs, 3 outputs). The
        // name buffers (Ort::AllocatedStringPtr) must outlive the Run call.
        std::vector<Ort::AllocatedStringPtr> inHolders, outHolders;
        std::vector<const char*> inNames, outNames;
        for (int i = 0; i < static_cast<int>(sess.GetInputCount()); ++i) {
            inHolders.push_back(sess.GetInputNameAllocated(i, alloc));
            inNames.push_back(inHolders.back().get());
        }
        auto tEmb   = Ort::Value::CreateTensor<float>(mem, embedding.data(), embedding.size(), embShape.data(), embShape.size());
        auto tCoord = Ort::Value::CreateTensor<float>(mem, coords.data(), coords.size(), coordShape.data(), coordShape.size());
        auto tLabel = Ort::Value::CreateTensor<float>(mem, labels.data(), labels.size(), labelShape.data(), labelShape.size());
        auto tMask  = Ort::Value::CreateTensor<float>(mem, const_cast<float*>(maskInput.data()), maskInput.size(), maskShape.data(), maskShape.size());
        auto tHas   = Ort::Value::CreateTensor<float>(mem, const_cast<float*>(hasMask.data()), hasMask.size(), hasShape.data(), hasShape.size());
        auto tOrig  = Ort::Value::CreateTensor<float>(mem, const_cast<float*>(origSz.data()), origSz.size(), origShape.data(), origShape.size());

        std::vector<Ort::Value> inVals;
        inVals.reserve(6);
        inVals.push_back(std::move(tEmb));
        inVals.push_back(std::move(tCoord));
        inVals.push_back(std::move(tLabel));
        inVals.push_back(std::move(tMask));
        inVals.push_back(std::move(tHas));
        inVals.push_back(std::move(tOrig));
        // Ort::Run requires a *contiguous* array of Ort::Value (not pointers);
        // the vector owns the six moved-in Values, so their addresses are
        // stable across the Run call.
        for (int i = 0; i < static_cast<int>(sess.GetOutputCount()); ++i) {
            outHolders.push_back(sess.GetOutputNameAllocated(i, alloc));
            outNames.push_back(outHolders.back().get());
        }
        auto outT = sess.Run(Ort::RunOptions{nullptr},
                             inNames.data(), inVals.data(), inVals.size(),
                             outNames.data(), outNames.size());

        // Identify the iou and mask outputs by scanning names.
        int iouIdx = -1, maskIdx = -1;
        int nOut = static_cast<int>(sess.GetOutputCount());
        {
            for (int i = 0; i < nOut; ++i) {
                std::string n(outNames[i]);
                if (n == "iou_predictions") iouIdx = i;
                else if (n == "masks") maskIdx = i;
            }
        }
        if (iouIdx < 0 || maskIdx < 0 || static_cast<size_t>(maskIdx) >= outT.size() ||
            static_cast<size_t>(iouIdx) >= outT.size()) {
            setLastError(lastError_, "runSamPoint: decoder missing expected outputs", errMutex_);
            return result;
        }

        // Read iou_predictions -> choose best candidate index per point (0 here).
        auto iouInfo = outT[static_cast<size_t>(iouIdx)].GetTensorTypeAndShapeInfo();
        auto iouDims = iouInfo.GetShape();
        const float* iou = outT[static_cast<size_t>(iouIdx)].GetTensorData<float>();
        size_t iouElems = iouInfo.GetElementCount();
        // M = number of candidate masks per point. The iou tensor is laid out
        // as [P, M], [1, P, M] or [B, P, M] depending on export, so M is the
        // *last* dimension (candidates are always the innermost non-batch axis).
        int M = 2;  // fallback for full SAM
        if (iouDims.size() >= 2)
            M = static_cast<int>(iouDims.back());
        if (M <= 0 && iouElems > 0)
            M = static_cast<int>(std::max<size_t>(1, iouElems / static_cast<size_t>(P)));
        if (M > static_cast<int>(iouElems))
            M = static_cast<int>(iouElems);  // guard against trailing batch dims
        int best = 0;
        for (int j = 1; j < M; ++j) {
            const size_t offset = std::min<size_t>(static_cast<size_t>(j), static_cast<size_t>(M) - 1);
            if (offset < iouElems && iou[offset] > iou[static_cast<size_t>(best)])
                best = j;
        }
        result.iouScore = (M > 0 && iouElems > 0) ? iou[static_cast<size_t>(best)] : 0.0f;

        // Read masks ([P, M, newH, newW] expected; model resized via orig_im_size).
        auto maskInfo = outT[static_cast<size_t>(maskIdx)].GetTensorTypeAndShapeInfo();
        auto maskDims = maskInfo.GetShape();
        const float* maskRaw = outT[static_cast<size_t>(maskIdx)].GetTensorData<float>();
        const size_t maskElems = maskInfo.GetElementCount();
        int mW = newW, mH = newH, lead = 1;
        // Derive (mH, mW) from the trailing two dims and the candidate axis from
        // the third-from-last. Handles [M,H,W], [1,M,H,W] and [1,P,M,H,W]
        // layouts: M is always innermost-before the spatial dims.
        if (maskDims.size() >= 3) {
            lead = static_cast<int>(maskDims[maskDims.size() - 3]);  // candidate axis
            mH   = static_cast<int>(maskDims[maskDims.size() - 2]);
            mW   = static_cast<int>(maskDims[maskDims.size() - 1]);
        }
        if (mW <= 0) mW = newW;
        if (mH <= 0) mH = newH;
        const size_t perMask = static_cast<size_t>(std::max(1, mW)) * std::max(1, mH);
        if (maskElems < perMask) {
            setLastError(lastError_, "runSamPoint: masks output too small", errMutex_);
            return result;
        }
        const int useIdx = (lead > 0 && best < lead) ? best : 0;
        std::vector<float> bestMask(maskRaw + static_cast<size_t>(useIdx) * perMask,
                                    maskRaw + static_cast<size_t>(useIdx) * perMask + perMask);

        // Sigmoid + resize the selected mask to original resolution.
        for (auto& v : bestMask) v = sigmoid(v);
        std::vector<float> full = resize1Ch(bestMask, mW, mH, origW, origH);

        result.width = origW; result.height = origH;
        result.mask = std::move(full);
        result.ok = true;
        return result;
    } catch (const Ort::Exception& e) {
        setLastError(lastError_, std::string("runSamPoint: ") + e.what(), errMutex_);
        return result;
    }
#else
    setLastError(lastError_, "runSamPoint: LRC_ENABLE_AI=0", errMutex_);
    return result;
#endif
}

// ---------------------------------------------------------------------------
// preprocessImage — always available (no ONNX dependency)
// ---------------------------------------------------------------------------
std::vector<float> AiInferenceEngine::preprocessImage(
    const ImageBuffer& input,
    int targetW, int targetH,
    const std::vector<float>& mean,
    const std::vector<float>& stdv) {
    const int w = input.width(), h = input.height();
    if (w <= 0 || h <= 0 || targetW <= 0 || targetH <= 0) return {};

    // Step 1: convert input to flat float RGB [0,1], HxW interleaved
    std::vector<float> src(static_cast<size_t>(w) * h * 3);
    if (input.format() == PixelFormat::F32) {
        const float* raw = reinterpret_cast<const float*>(input.data().data());
        for (size_t i = 0; i < src.size(); ++i) src[i] = raw[i];
    } else if (input.format() == PixelFormat::U16) {
        const uint16_t* raw = reinterpret_cast<const uint16_t*>(input.data().data());
        const float inv = 1.0f / 65535.0f;
        for (size_t i = 0; i < src.size(); ++i)
            src[i] = static_cast<float>(raw[i]) * inv;
    } else { // U8
        const uint8_t* raw = input.data().data();
        const float inv = 1.0f / 255.0f;
        for (size_t i = 0; i < src.size(); ++i)
            src[i] = static_cast<float>(raw[i]) * inv;
    }

    // Step 2: resize to targetW x targetH (float RGB interleaved)
    std::vector<float> resized = resizeFloatRGB(src.data(), w, h, targetW, targetH);

    // Step 3: NHWC -> NCHW + mean/std norm
    const float m0 = mean.size() > 0 ? mean[0] : 0.0f;
    const float m1 = mean.size() > 1 ? mean[1] : 0.0f;
    const float m2 = mean.size() > 2 ? mean[2] : 0.0f;
    const float s0 = stdv.size() > 0 ? (std::fabs(stdv[0]) > 1e-6f ? stdv[0] : 1.0f) : 1.0f;
    const float s1 = stdv.size() > 1 ? (std::fabs(stdv[1]) > 1e-6f ? stdv[1] : 1.0f) : 1.0f;
    const float s2 = stdv.size() > 2 ? (std::fabs(stdv[2]) > 1e-6f ? stdv[2] : 1.0f) : 1.0f;

    const size_t N = static_cast<size_t>(targetW) * targetH;
    std::vector<float> out(N * 3);
    for (int y = 0; y < targetH; ++y) {
        for (int x = 0; x < targetW; ++x) {
            const size_t px = (static_cast<size_t>(y) * targetW + x) * 3;
            const float r = resized[px];
            const float g = resized[px + 1];
            const float b = resized[px + 2];
            const size_t base = static_cast<size_t>(y) * targetW + x;
            out[base              ] = (r - m0) / s0;
            out[base + N          ] = (g - m1) / s1;
            out[base + N * 2      ] = (b - m2) / s2;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// postprocessImage — always available
// ---------------------------------------------------------------------------
ImageBufferPtr AiInferenceEngine::postprocessImage(
    const std::vector<float>& tensor,
    int w, int h, int channels,
    PixelFormat outFormat,
    const std::vector<float>& mean,
    const std::vector<float>& stdv) {
    if (w <= 0 || h <= 0 || channels <= 0) return nullptr;
    // Assume 3 channels for the final buffer; if 1-channel (mask) replicate R.
    const int outCh = 3;
    auto buf = ImageBuffer::create(w, h, outFormat);
    const size_t nPx = static_cast<size_t>(w) * h;

    const float m0 = mean.size() > 0 ? mean[0] : 0.0f;
    const float m1 = mean.size() > 1 ? mean[1] : 0.0f;
    const float m2 = mean.size() > 2 ? mean[2] : 0.0f;
    const float s0 = stdv.size() > 0 ? stdv[0] : 1.0f;
    const float s1 = stdv.size() > 1 ? stdv[1] : 1.0f;
    const float s2 = stdv.size() > 2 ? stdv[2] : 1.0f;

    // Tensor layout is assumed NCHW (1, C, H, W) — which is what
    // preprocessImage emits.  If channels != 3 or the tensor size suggests
    // a different layout we fall back to a simple copy.
    const bool isNchw3Ch = (tensor.size() == nPx * 3 && channels >= 3);
    const bool isNchw1Ch = (tensor.size() >= nPx && channels == 1);

    auto clamp = [](float v) {
        if (v < 0.0f) return 0.0f;
        if (v > 1.0f) return 1.0f;
        return v;
    };

    if (outFormat == PixelFormat::F32) {
        float* dst = reinterpret_cast<float*>(buf->data().data());
        if (isNchw3Ch) {
            for (size_t i = 0; i < nPx; ++i) {
                const float r = clamp(tensor[i        ] * s0 + m0);
                const float g = clamp(tensor[i + nPx  ] * s1 + m1);
                const float b = clamp(tensor[i + nPx*2] * s2 + m2);
                const size_t di = i * 3;
                dst[di]     = r;
                dst[di + 1] = g;
                dst[di + 2] = b;
            }
        } else if (isNchw1Ch) {
            // 1-channel mask -> replicate to R,G,B so display pipelines treat
            // it as a grey-scale weight image.
            for (size_t i = 0; i < nPx; ++i) {
                const float m = clamp(tensor[i]);
                const size_t di = i * 3;
                dst[di] = dst[di + 1] = dst[di + 2] = m;
            }
        } else {
            // Generic copy (interleaved assumed)
            for (size_t i = 0; i < nPx; ++i) {
                size_t si = i * std::max(1, channels);
                size_t di = i * 3;
                for (int c = 0; c < outCh; ++c) {
                    const float v = clamp(tensor[si + std::min(c, channels - 1)]);
                    dst[di + c] = v;
                }
            }
        }
    } else { // U8 or U16
        auto& raw = buf->data();
        if (outFormat == PixelFormat::U8) {
            for (size_t i = 0; i < nPx; ++i) {
                float r, g, b;
                if (isNchw3Ch) {
                    r = clamp(tensor[i        ] * s0 + m0);
                    g = clamp(tensor[i + nPx  ] * s1 + m1);
                    b = clamp(tensor[i + nPx*2] * s2 + m2);
                } else if (isNchw1Ch) {
                    r = g = b = clamp(tensor[i]);
                } else {
                    size_t si = i * std::max(1, channels);
                    r = clamp(tensor[si]);
                    g = clamp(tensor[si + (channels > 1 ? 1 : 0)]);
                    b = clamp(tensor[si + (channels > 2 ? 2 : 0)]);
                }
                const size_t di = i * 3;
                raw[di]     = static_cast<uint8_t>(r * 255.0f + 0.5f);
                raw[di + 1] = static_cast<uint8_t>(g * 255.0f + 0.5f);
                raw[di + 2] = static_cast<uint8_t>(b * 255.0f + 0.5f);
            }
        } else { // U16
            uint16_t* dst = reinterpret_cast<uint16_t*>(raw.data());
            for (size_t i = 0; i < nPx; ++i) {
                float r, g, b;
                if (isNchw3Ch) {
                    r = clamp(tensor[i        ] * s0 + m0);
                    g = clamp(tensor[i + nPx  ] * s1 + m1);
                    b = clamp(tensor[i + nPx*2] * s2 + m2);
                } else {
                    r = g = b = clamp(tensor[i]);
                }
                const size_t di = i * 3;
                dst[di]     = static_cast<uint16_t>(r * 65535.0f + 0.5f);
                dst[di + 1] = static_cast<uint16_t>(g * 65535.0f + 0.5f);
                dst[di + 2] = static_cast<uint16_t>(b * 65535.0f + 0.5f);
            }
        }
    }
    return buf;
}

} // namespace lrc
