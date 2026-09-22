#pragma once

// P3-4 D3D11 DirectCompute spatial-filter backend. Windows-only, Qt-free.
// Compiled only when CMake option LRC_ENABLE_D3D=ON (the header's include
// sites are guarded by the same macro), so default builds never pull in
// d3d11.h / windows.h through this file.
//
// Design: a separable two-pass gaussian blur (horizontal + vertical compute
// dispatches) over R32_FLOAT buffers. The HLSL is embedded as a string and
// compiled at runtime with D3DCompile, so no .hlsl build step is needed.
// Kernel selection mirrors the CPU implementation: sigma <= 4 uses the exact
// separable kernel (radius = ceil(3*sigma)), larger sigma uses three
// sliding-window box passes. Every dispatch pair is followed by a synchronous
// readback (staging buffer + Map), so blur() returns only when `out` is
// fully written.

#include <memory>

#include "engine/SpatialFilter.h"

namespace lrc {

/**
 * @brief D3D11 DirectCompute implementation of SpatialFilterBackend.
 *
 * Created via tryCreate(), which probes for a D3D11 device. The class is a
 * pimpl wrapper so SpatialFilterD3D11.h stays free of <d3d11.h>.
 *
 * @note Thread-safety: the D3D11 immediate context is not thread-safe, so all
 *       public methods serialize through an internal mutex. Blur is fully
 *       synchronous (Map readback) -- no out-of-order completion.
 */
class D3D11SpatialFilterBackend final : public SpatialFilterBackend {
public:
    /**
     * @brief Probes for a D3D11 device and builds the DirectCompute backend.
     *
     * Tries the hardware adapter first; with allowWarp=true also accepts the
     * WARP software rasterizer (no speedup, intended for tests/CI on machines
     * without a GPU). Device creation is SEH-protected: virtualized hosts may
     * raise an access violation instead of returning an HRESULT, which is
     * treated as "GPU unavailable". A tiny compute dispatch then validates that
     * the device actually executes shaders and returns sane data (some hosts
     * hand out a Basic Render Driver that crashes or produces garbage on real
     * work); that too degrades to "GPU unavailable".
     *
     * @param allowWarp Also accept the WARP driver.
     * @return A ready backend, or null when no usable device exists.
     */
    static std::shared_ptr<const SpatialFilterBackend> tryCreate(bool allowWarp = false);

    ~D3D11SpatialFilterBackend() override;

    /// @copydoc SpatialFilterBackend::blur
    void blur(const float* src, int w, int h, float sigma,
              std::vector<float>& out) const override;

    bool isGpu() const noexcept override { return true; }

    /// @return "d3d11" (or "d3d11-warp" when running on the software device).
    const char* name() const noexcept override;

private:
    D3D11SpatialFilterBackend() = default;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lrc
