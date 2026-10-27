// P3-4: CPU spatial-filter backend + backend factory + process-wide accessor.
//
// The CPU backend wraps the legacy Pipeline.cpp gaussianBlur via the
// detail::gaussianBlur bridge, so default builds are byte-identical to the
// pre-P3-4 code. The factory probes the D3D11 DirectCompute backend first when
// the build is compiled with LRC_ENABLE_D3D=ON and falls back to CPU on any
// failure.

#include "engine/SpatialFilter.h"

#include <atomic>
#include <mutex>
#include <utility>

#include "engine/GaussianBlur.h"

#if defined(LRC_ENABLE_D3D)
#include "engine/SpatialFilterD3D11.h"
#endif

namespace lrc {

namespace {

// Stateless CPU backend: delegates to the legacy implementation. Safe to share
// across threads (detail::gaussianBlur only touches thread-local scratch).
class CpuSpatialFilterBackend final : public SpatialFilterBackend {
public:
    void blur(const float* src, int w, int h, float sigma,
              std::vector<float>& out) const override {
        detail::gaussianBlur(src, w, h, sigma, out);
    }
};

// Requested backend preference (process-global). Relaxed ordering is enough:
// the value is only read to decide which backend to construct under the
// instance mutex below, never to synchronise data.
std::atomic<PixelBackendPreference> g_preference{PixelBackendPreference::Auto};

// Process-wide pipeline backend instance. The atomic shared_ptr gives render
// call sites a lock-free fast path while setPixelBackendPreference() may swap
// the instance from the UI thread; any in-flight render keeps its own
// shared_ptr copy, so a swap cannot invalidate a running blur.
std::atomic<std::shared_ptr<const SpatialFilterBackend>> g_pipelineBackend;

std::mutex g_backendMutex;

}  // namespace

std::shared_ptr<const SpatialFilterBackend> cpuSpatialFilterBackend() {
    // Function-local static: initialized once, thread-safe (C++11 magic
    // statics), and destructed at process exit after the callers that hold a
    // copy have released theirs.
    static const std::shared_ptr<const SpatialFilterBackend> inst =
        std::make_shared<CpuSpatialFilterBackend>();
    return inst;
}

std::shared_ptr<const SpatialFilterBackend> createSpatialFilterBackend() {
    // Forced CPU is an explicit user preference: skip the device probe entirely
    // so a broken driver can never be touched (see the preference contract in
    // SpatialFilter.h).
    if (g_preference.load(std::memory_order_relaxed) ==
        PixelBackendPreference::ForceCpu) {
        return cpuSpatialFilterBackend();
    }
#if defined(LRC_ENABLE_D3D)
    // Hardware only: WARP has no speed advantage over the CPU backend, so the
    // production factory never picks it (tests may create a WARP backend
    // explicitly to exercise the DirectCompute path on GPU-less machines).
    if (std::shared_ptr<const SpatialFilterBackend> gpu =
            D3D11SpatialFilterBackend::tryCreate(/*allowWarp=*/false)) {
        return gpu;
    }
#endif
    return cpuSpatialFilterBackend();
}

std::shared_ptr<const SpatialFilterBackend> pipelineSpatialFilterBackend() {
    // Fast path: one atomic shared_ptr load per blur call site invocation.
    if (std::shared_ptr<const SpatialFilterBackend> inst =
            g_pipelineBackend.load(std::memory_order_acquire)) {
        return inst;
    }
    // Slow path: the probe (device creation) runs once, under the mutex; after
    // that the instance is stable until the preference is changed.
    std::lock_guard<std::mutex> lock(g_backendMutex);
    std::shared_ptr<const SpatialFilterBackend> inst =
        g_pipelineBackend.load(std::memory_order_relaxed);
    if (!inst) {
        inst = createSpatialFilterBackend();
        g_pipelineBackend.store(inst, std::memory_order_release);
    }
    return inst;
}

void setPixelBackendPreference(PixelBackendPreference preference) noexcept {
    g_preference.store(preference, std::memory_order_relaxed);
    try {
        std::lock_guard<std::mutex> lock(g_backendMutex);
        g_pipelineBackend.store(createSpatialFilterBackend(),
                                std::memory_order_release);
    } catch (...) {
        // Out of memory while rebuilding: keep the previous instance. The
        // preference is already stored, so a later accessor call retries.
    }
}

PixelBackendPreference pixelBackendPreference() noexcept {
    return g_preference.load(std::memory_order_relaxed);
}

const char* activeSpatialBackendName() noexcept {
    return pipelineSpatialFilterBackend()->name();
}

}  // namespace lrc
