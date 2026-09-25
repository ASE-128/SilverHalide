#pragma once

// CPU/GPU-switchable spatial-filter backend.
//
// Pipeline spatial ops (gaussian blur and the sharpening that composes from it)
// run through this abstraction instead of calling the file-local gaussianBlur
// directly. At runtime `createSpatialFilterBackend()` returns a DirectCompute
// (D3D11) backend when the build is compiled with LRC_ENABLE_D3D=ON AND the
// device probe succeeds; otherwise it returns the CPU backend, which wraps the
// legacy Pipeline.cpp gaussianBlur unchanged. The pipeline therefore keeps its
// exact behaviour of the legacy path unless a GPU backend was explicitly
// compiled in and successfully initialized.
//
// All backend methods are const / thread-safe (the GPU backend serializes
// through an internal mutex; the CPU backend is stateless).

#include <memory>
#include <vector>

namespace lrc {

#if defined(LRC_ENABLE_D3D)
class D3D11SpatialFilterBackend;
#endif

/**
 * @brief Abstract spatial-filter backend (CPU scalar/AVX2 vs D3D11 DirectCompute).
 *
 * The minimal operator surface is a single-channel gaussian blur;
 * sharpening is composed in the pipeline as luma minus the blurred field, so it
 * consumes the same backend op. The contract mirrors the legacy gaussianBlur
 * exactly (see detail::gaussianBlur), so any backend is a drop-in replacement.
 *
 * @note Thread-safety: implementations must be safe to call from concurrent
 *       render threads. The GPU backend serializes dispatch + readback under a
 *       mutex; the CPU backend is stateless.
 */
class SpatialFilterBackend {
public:
    virtual ~SpatialFilterBackend() = default;

    /**
     * @brief Single-channel separable gaussian blur.
     *
     * @param src   Source buffer, w*h floats.
     * @param w     Image width in pixels (> 0).
     * @param h     Image height in pixels (> 0).
     * @param sigma Gaussian sigma in pixels (>= 0; <= 0 copies).
     * @param out   Caller-owned output buffer; resized to w*h.
     *
     * @note The output is fully ready when the call returns (the GPU backend
     *       synchronously reads back through a staging buffer + Map before
     *       returning, so callers need no explicit flush/join).
     */
    virtual void blur(const float* src, int w, int h, float sigma,
                      std::vector<float>& out) const = 0;

    /// @return True when the backend executes on a GPU device (vs the CPU).
    virtual bool isGpu() const noexcept { return false; }

    /// @return Stable short backend identifier for diagnostics ("cpu"/"d3d11").
    virtual const char* name() const noexcept { return "cpu"; }
};

/**
 * @brief Runtime-selected backend (GPU when compiled in and probing succeeds).
 *
 * Never returns null: any GPU failure (macro off, device creation failure,
 * shader compile failure, or a sandboxed host that crashes during device
 * creation -- caught via SEH) degrades to the CPU backend.
 *
 * @return Shared backend instance; callers must not cache the result across
 *         long lifetimes for behaviour control (the process-wide pipeline
 *         accessor below holds the single instance).
 */
std::shared_ptr<const SpatialFilterBackend> createSpatialFilterBackend();

/// @return The always-available CPU backend (wraps detail::gaussianBlur).
std::shared_ptr<const SpatialFilterBackend> cpuSpatialFilterBackend();

/**
 * @brief Process-wide backend used by the pipeline's blur call sites.
 *
 * Lazily created on first use (thread-safe). The probe runs once; after that
 * the returned backend is stable for the process lifetime.
 *
 * @return Never null.
 */
std::shared_ptr<const SpatialFilterBackend> pipelineSpatialFilterBackend();

}  // namespace lrc
