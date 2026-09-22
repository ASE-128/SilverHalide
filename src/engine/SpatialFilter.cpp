// P3-4: CPU spatial-filter backend + backend factory + process-wide accessor.
//
// The CPU backend wraps the legacy Pipeline.cpp gaussianBlur via the
// detail::gaussianBlur bridge, so default builds are byte-identical to the
// pre-P3-4 code. The factory probes the D3D11 DirectCompute backend first when
// the build is compiled with LRC_ENABLE_D3D=ON and falls back to CPU on any
// failure.

#include "engine/SpatialFilter.h"

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
    // Lazily probed once per process; the probe (device creation) may take a
    // few milliseconds on first use, after which the instance is stable.
    static const std::shared_ptr<const SpatialFilterBackend> inst =
        createSpatialFilterBackend();
    return inst;
}

}  // namespace lrc
