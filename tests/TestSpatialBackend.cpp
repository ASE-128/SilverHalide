// P3-4 GPU pixel pipeline acceptance: spatial-filter backend abstraction.
//
// Verifies:
//   1. CPU backend is a drop-in for the legacy gaussianBlur (bit-identical,
//      plus a block-wise tolerance check against an independent reference).
//   2. The runtime-selected backend (factory) never returns null and its blur
//      stays within the P2-2 tolerance convention (Δ<0.008) no matter whether
//      the probe picked CPU or GPU.
//   3. When compiled with LRC_ENABLE_D3D=ON and a D3D11 device (hardware or
//      WARP) is available, the DirectCompute backend matches the CPU backend
//      block-wise within the same tolerance; otherwise it reports INFO and
//      skips (GPU speed-up measurement is a real-machine acceptance item).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "engine/Pipeline.h"
#include "engine/SpatialFilter.h"

#if defined(LRC_ENABLE_D3D)
#include "engine/SpatialFilterD3D11.h"
#endif

using namespace lrc;

namespace lrc {
// Test-only hook into the internal single-channel blur (defined in
// Pipeline.cpp; see tests/TestPipeline.cpp for the original declaration).
std::vector<float> pipelineGaussianBlurTest(const float* src, int w, int h,
                                            float sigma);
}

namespace {

// Smooth diagonal gradient spanning [0,1] (same construction as TestPipeline's
// makeBlurGradient): smooth everywhere, no clipping surprises.
std::vector<float> makeGradient(int w, int h) {
    std::vector<float> g(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            g[static_cast<size_t>(y) * w + x] =
                static_cast<float>(x + y) / static_cast<float>(w + h - 2);
    return g;
}

// Independent exact separable-gaussian reference (scalar, straightforward):
// kernel = exp(-0.5*(i/sigma)^2) normalized, radius = max(1, ceil(3*sigma)),
// two clamped passes. Matches the exact path of the production blur.
std::vector<float> exactGaussianRef(const float* src, int w, int h, float sigma) {
    const int radius = std::max(1, static_cast<int>(std::ceil(sigma * 3.0f)));
    std::vector<float> kernel(static_cast<size_t>(radius) * 2 + 1);
    float sum = 0.0f;
    for (int i = -radius; i <= radius; ++i) {
        const float v = std::exp(-0.5f * (static_cast<float>(i) / sigma) *
                                 (static_cast<float>(i) / sigma));
        kernel[static_cast<size_t>(i + radius)] = v;
        sum += v;
    }
    for (float& k : kernel)
        k /= sum;

    std::vector<float> tmp(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int i = -radius; i <= radius; ++i)
                acc += src[static_cast<size_t>(y) * w +
                           static_cast<size_t>(std::clamp(x + i, 0, w - 1))] *
                       kernel[static_cast<size_t>(i + radius)];
            tmp[static_cast<size_t>(y) * w + x] = acc;
        }
    std::vector<float> out(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int i = -radius; i <= radius; ++i)
                acc += tmp[static_cast<size_t>(
                           std::clamp(y + i, 0, h - 1)) * w + x] *
                       kernel[static_cast<size_t>(i + radius)];
            out[static_cast<size_t>(y) * w + x] = acc;
        }
    return out;
}

// Block-wise (64x64 tile) max-delta comparison; returns the worst tile's max
// delta. Both buffers must be w*h floats with w/h > 0.
float blockWiseMaxDelta(const std::vector<float>& a, const std::vector<float>& b,
                        int w, int h) {
    REQUIRE(a.size() == b.size());
    float worst = 0.0f;
    constexpr int kTile = 64;
    for (int ty = 0; ty < h; ty += kTile) {
        for (int tx = 0; tx < w; tx += kTile) {
            float tileMax = 0.0f;
            const int yEnd = std::min(h, ty + kTile);
            const int xEnd = std::min(w, tx + kTile);
            for (int y = ty; y < yEnd; ++y)
                for (int x = tx; x < xEnd; ++x) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    tileMax =
                        std::max(tileMax, std::abs(a[i] - b[i]));
                }
            worst = std::max(worst, tileMax);
        }
    }
    return worst;
}

// Shared sigmas: two exact-path sizes + the exact/box boundary + a box-path
// sigma, exercising unlike tap counts and both production branches.
constexpr float kSigmas[] = {0.8f, 2.5f, 4.0f, 8.0f};

}  // namespace

TEST_CASE("Spatial backend: CPU backend is bit-identical to legacy gaussianBlur",
          "[spatial][backend][cpu]") {
    const int w = 96, h = 64;
    const std::vector<float> grad = makeGradient(w, h);

    const auto cpu = cpuSpatialFilterBackend();
    REQUIRE(cpu != nullptr);
    REQUIRE_FALSE(cpu->isGpu());

    for (const float sigma : kSigmas) {
        std::vector<float> viaBackend;
        cpu->blur(grad.data(), w, h, sigma, viaBackend);
        const std::vector<float> viaLegacy =
            pipelineGaussianBlurTest(grad.data(), w, h, sigma);
        REQUIRE(viaBackend.size() == viaLegacy.size());
        REQUIRE(viaBackend == viaLegacy);  // bit-identical wrapper contract
    }
}

TEST_CASE("Spatial backend: CPU backend matches exact reference block-wise",
          "[spatial][backend][cpu]") {
    const int w = 160, h = 96;
    const std::vector<float> grad = makeGradient(w, h);

    const auto cpu = cpuSpatialFilterBackend();
    for (const float sigma : kSigmas) {
        std::vector<float> out;
        cpu->blur(grad.data(), w, h, sigma, out);
        // P2-2 convention: reassociated float math stays within Δ<0.008 of the
        // scalar reference (scalar build is effectively 0, SIMD/AVX2 ~1e-7).
        if (sigma <= 4.0f) {  // reference is the exact separable conv
            const std::vector<float> ref =
                exactGaussianRef(grad.data(), w, h, sigma);
            const float delta = blockWiseMaxDelta(out, ref, w, h);
            CAPTURE(sigma, delta);
            REQUIRE(delta < 0.008f);
        }
        // Box path (sigma 8): bounded [0,1] output (weighted average).
        for (const float v : out) {
            REQUIRE(v >= 0.0f);
            REQUIRE(v <= 1.0f);
        }
    }
}

TEST_CASE("Spatial backend: degenerate inputs behave like the legacy blur",
          "[spatial][backend][cpu]") {
    const int w = 32, h = 32;
    const std::vector<float> grad = makeGradient(w, h);

    const auto cpu = cpuSpatialFilterBackend();

    // sigma <= 0 -> exact copy.
    std::vector<float> out;
    cpu->blur(grad.data(), w, h, 0.0f, out);
    REQUIRE(out == grad);

    // Empty / invalid dimensions -> empty output, no crash.
    std::vector<float> empty;
    cpu->blur(grad.data(), 0, 0, 2.5f, empty);
    REQUIRE(empty.empty());
}

TEST_CASE("Spatial backend: factory never returns null and stays in tolerance",
          "[spatial][backend][fallback]") {
    // Runtime selection: GPU (compiled in + device probe OK) or CPU fallback.
    // The output must satisfy the P2-2 tolerance either way, which is what the
    // pipeline relies on when the render hot path uses the backend.
    const auto backend = createSpatialFilterBackend();
    REQUIRE(backend != nullptr);
    INFO("selected backend: " << backend->name());

    // The process-wide pipeline accessor is stable and non-null.
    const auto pipelineBackend = pipelineSpatialFilterBackend();
    REQUIRE(pipelineBackend != nullptr);
    REQUIRE(pipelineBackend == pipelineSpatialFilterBackend());

#if !defined(LRC_ENABLE_D3D)
    // Default build (no GPU compiled in) must resolve to the CPU backend.
    REQUIRE_FALSE(backend->isGpu());
#endif

    const int w = 128, h = 128;
    const std::vector<float> grad = makeGradient(w, h);
    for (const float sigma : kSigmas) {
        std::vector<float> out;
        backend->blur(grad.data(), w, h, sigma, out);
        REQUIRE(out.size() == static_cast<size_t>(w) * h);
        if (sigma <= 4.0f) {
            const std::vector<float> ref =
                exactGaussianRef(grad.data(), w, h, sigma);
            const float delta = blockWiseMaxDelta(out, ref, w, h);
            CAPTURE(sigma, delta);
            REQUIRE(delta < 0.008f);
        }
    }
}

TEST_CASE("Spatial backend: ForceCpu preference short-circuits the GPU probe",
          "[spatial][backend][preference]") {
    // The preference is process-global; collect the observations first and
    // restore Auto before asserting so a failed REQUIRE cannot leak ForceCpu
    // into the remaining tests of this binary.
    setPixelBackendPreference(PixelBackendPreference::ForceCpu);
    const bool forcedIsCpu = !pipelineSpatialFilterBackend()->isGpu();
    const std::string forcedName = activeSpatialBackendName();
    const bool factoryIsCpu = !createSpatialFilterBackend()->isGpu();

    setPixelBackendPreference(PixelBackendPreference::Auto);
    const bool autoRestored =
        pixelBackendPreference() == PixelBackendPreference::Auto;
    const bool autoBackendAlive = pipelineSpatialFilterBackend() != nullptr;

    REQUIRE(forcedIsCpu);
    REQUIRE(forcedName == "cpu");
    REQUIRE(factoryIsCpu);
    REQUIRE(autoRestored);
    REQUIRE(autoBackendAlive);
}

#if defined(LRC_ENABLE_D3D)
TEST_CASE("Spatial backend: D3D11 DirectCompute matches CPU block-wise",
          "[spatial][backend][gpu]") {
    // allowWarp=true so GPU-less CI machines can still exercise the
    // DirectCompute path (WARP has no speed benefit; production selection is
    // hardware-only via createSpatialFilterBackend).
    const auto gpu = D3D11SpatialFilterBackend::tryCreate(/*allowWarp=*/true);
    if (!gpu) {
        SKIP("D3D11 device unavailable on this host; GPU path not exercised");
    }
    INFO("GPU backend: " << gpu->name());
    REQUIRE(gpu->isGpu());

    const auto cpu = cpuSpatialFilterBackend();

    // Non-square sizes also cover the flattened 1D indexing (dispatch groups
    // are derived from w*h, not from w or h alone).
    const int sizes[][2] = {{512, 512}, {640, 480}};
    for (const auto& wh : sizes) {
        const int w = wh[0], h = wh[1];
        const std::vector<float> grad = makeGradient(w, h);
        for (const float sigma : kSigmas) {
            std::vector<float> gpuOut, cpuOut;
            gpu->blur(grad.data(), w, h, sigma, gpuOut);
            cpu->blur(grad.data(), w, h, sigma, cpuOut);
            REQUIRE(gpuOut.size() == cpuOut.size());
            const float delta = blockWiseMaxDelta(gpuOut, cpuOut, w, h);
            CAPTURE(sigma, w, h, delta);
            REQUIRE(delta < 0.008f);
        }
    }
}
#endif  // LRC_ENABLE_D3D
