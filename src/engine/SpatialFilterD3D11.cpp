// P3-4 D3D11 DirectCompute spatial-filter backend (compiled only with
// LRC_ENABLE_D3D=ON). Windows-only, Qt-free.
//
// The gaussian blur runs as a separable two-pass compute dispatch over
// R32_FLOAT ping-pong buffers; the HLSL is embedded below and compiled at
// runtime with D3DCompile (no .hlsl build step). Kernel selection mirrors the
// CPU implementation in Pipeline.cpp so outputs stay within float tolerance:
//   sigma <= 4   -> exact separable kernel, radius = ceil(3*sigma)
//   sigma >  4   -> three box passes (wBox = nearest odd sqrt(12s^2/3+1))
// After the last dispatch the result is copied to a STAGING buffer and Map'd
// back, so blur() is fully synchronous: `out` is ready when it returns.
//
// Thread-safety: the D3D11 immediate context is single-threaded, so all public
// calls serialize through Impl::mtx. Ping-pong buffers and the staging buffer
// are lazily (re)allocated when a larger image arrives; the constant buffer is
// updated per call.

#include "engine/SpatialFilterD3D11.h"

#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <utility>

namespace lrc {

namespace {

using Microsoft::WRL::ComPtr;

// Compute shader target is cs_5_0 (feature level 11.0); probe the device at
// 11.0 with 10.x fallback so WARP-less older hardware still initializes.
constexpr D3D_FEATURE_LEVEL kFeatureLevels[] = {
    D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0,
};

// Embedded compute shader. Two entry points share one constant buffer:
// BlurH / BlurV run either the exact separable convolution (gMode == 0,
// weights in gKernel) or a uniform box window (gMode == 1). Samples are
// coordinate-clamped at the image edges, matching the CPU implementation.
const char* const kHlsl = R"HLSL(
RWBuffer<float> gOut : register(u0);
Buffer<float>   gIn  : register(t0);

cbuffer gParams : register(b0) {
    uint  gW;         // image width
    uint  gH;         // image height
    uint  gRadius;    // tap radius (kernel half-width)
    float gInvBox;    // 1 / box width (box mode only)
    uint  gMode;      // 0 = exact separable, 1 = box
    uint  gPad0;
    uint  gPad1;
    uint  gPad2;
    float gKernel[64]; // exact-kernel weights, indexed [0 .. 2*gRadius]
};

[numthreads(256, 1, 1)]
void BlurH(uint3 dtid : SV_DispatchThreadID) {
    uint idx = dtid.y * 16776960u + dtid.x;  // kThreadsPerRow, mirrored in C++
    if (idx >= gW * gH) return;
    uint x = idx % gW;
    uint y = idx / gW;
    float acc = 0.0;
    int r = int(gRadius);
    if (gMode == 0u) {
        for (int i = -r; i <= r; ++i) {
            int sx = clamp(int(x) + i, 0, int(gW) - 1);
            acc += gIn[uint(sx) + y * gW] * gKernel[uint(i + r)];
        }
    } else {
        for (int i = -r; i <= r; ++i) {
            int sx = clamp(int(x) + i, 0, int(gW) - 1);
            acc += gIn[uint(sx) + y * gW];
        }
        acc *= gInvBox;
    }
    gOut[idx] = acc;
}

[numthreads(256, 1, 1)]
void BlurV(uint3 dtid : SV_DispatchThreadID) {
    uint idx = dtid.y * 16776960u + dtid.x;
    if (idx >= gW * gH) return;
    uint x = idx % gW;
    uint y = idx / gW;
    float acc = 0.0;
    int r = int(gRadius);
    if (gMode == 0u) {
        for (int i = -r; i <= r; ++i) {
            int sy = clamp(int(y) + i, 0, int(gH) - 1);
            acc += gIn[uint(sy) * gW + x] * gKernel[uint(i + r)];
        }
    } else {
        for (int i = -r; i <= r; ++i) {
            int sy = clamp(int(y) + i, 0, int(gH) - 1);
            acc += gIn[uint(sy) * gW + x];
        }
        acc *= gInvBox;
    }
    gOut[idx] = acc;
}
)HLSL";

// Constant-buffer layout must match the HLSL cbuffer above byte-for-byte.
struct alignas(16) Params {
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t radius = 0;
    float invBox = 0.0f;
    uint32_t mode = 0;  // 0 = exact separable, 1 = box
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
    uint32_t pad2 = 0;
    float kernel[64] = {};
};

// SEH-protected device creation. Some virtualized / sandboxed hosts raise an
// access violation inside the D3D11 runtime instead of returning an HRESULT;
// /EHsc forbids __try in functions containing C++ objects needing unwinding,
// so this isolated helper holds no C++ locals and translates any crash into a
// plain failure code.
HRESULT createDeviceSeh(D3D_DRIVER_TYPE driverType, bool singleThreaded,
                        ID3D11Device** device, ID3D11DeviceContext** context) {
    __try {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        if (singleThreaded) {
            // All backend calls serialize under a mutex, so the single-threaded
            // flag (faster internal sync) is valid here.
            flags |= D3D11_CREATE_DEVICE_SINGLETHREADED;
        }
        return D3D11CreateDevice(nullptr, driverType, nullptr, flags,
                                 kFeatureLevels,
                                 static_cast<UINT>(std::size(kFeatureLevels)),
                                 D3D11_SDK_VERSION, device, nullptr, context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

// Compiles one shader entry point; returns null on failure (diagnostics to the
// debug output only, so headless fallback stays silent).
ComPtr<ID3D11ComputeShader> compileShader(ID3D11Device* device,
                                          const char* entry) {
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(kHlsl, std::strlen(kHlsl), "gaussian_blur.hlsl",
                                  nullptr, nullptr, entry, "cs_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                  &blob, &errors);
    if (FAILED(hr)) {
        if (errors && errors->GetBufferPointer() && errors->GetBufferSize() > 0) {
            OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        }
        return {};
    }
    ComPtr<ID3D11ComputeShader> shader;
    if (FAILED(device->CreateComputeShader(
            blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader))) {
        return {};
    }
    return shader;
}

}  // namespace

struct D3D11SpatialFilterBackend::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> shaderH;  // BlurH (exact + box)
    ComPtr<ID3D11ComputeShader> shaderV;  // BlurV (exact + box)
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11Buffer> bufA;  // ping-pong buffers (SRV | UAV)
    ComPtr<ID3D11Buffer> bufB;
    ComPtr<ID3D11ShaderResourceView> srvA;
    ComPtr<ID3D11ShaderResourceView> srvB;
    ComPtr<ID3D11UnorderedAccessView> uavA;
    ComPtr<ID3D11UnorderedAccessView> uavB;
    ComPtr<ID3D11Buffer> staging;  // CPU-read copy target for the final result
    size_t bufBytes = 0;           // current allocation size of the buffers
    bool warp = false;

    // (Re)allocates the ping-pong + staging buffers when the image grew beyond
    // the current allocation. Called with the mutex held.
    bool ensureBuffers(size_t bytes) {
        if (bytes <= bufBytes && bufA) {
            return true;
        }
        srvA.Reset();
        srvB.Reset();
        uavA.Reset();
        uavB.Reset();
        bufA.Reset();
        bufB.Reset();
        staging.Reset();

        D3D11_BUFFER_DESC bufDesc{};
        bufDesc.ByteWidth = static_cast<UINT>(bytes);
        bufDesc.Usage = D3D11_USAGE_DEFAULT;
        bufDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(device->CreateBuffer(&bufDesc, nullptr, &bufA)) ||
            FAILED(device->CreateBuffer(&bufDesc, nullptr, &bufB))) {
            return false;
        }

        D3D11_BUFFER_DESC stageDesc{};
        stageDesc.ByteWidth = static_cast<UINT>(bytes);
        stageDesc.Usage = D3D11_USAGE_STAGING;
        stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateBuffer(&stageDesc, nullptr, &staging))) {
            return false;
        }

        const UINT numElems = static_cast<UINT>(bytes / sizeof(float));
        for (int i = 0; i < 2; ++i) {
            ID3D11Buffer* buf = i == 0 ? bufA.Get() : bufB.Get();
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = numElems;
            if (FAILED(device->CreateShaderResourceView(buf, &srvDesc,
                                                        i == 0 ? &srvA : &srvB))) {
                return false;
            }
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.FirstElement = 0;
            uavDesc.Buffer.NumElements = numElems;
            if (FAILED(device->CreateUnorderedAccessView(buf, &uavDesc,
                                                         i == 0 ? &uavA : &uavB))) {
                return false;
            }
        }

        bufBytes = bytes;
        return true;
    }

    // Runs one blur pass: reads the resource behind `inSrv`, writes the
    // resource behind `outUav`. Unbinds both views afterwards so the next pass
    // can rebind the same buffers on the other role without a D3D11 binding
    // hazard.
    void runPass(ID3D11ComputeShader* shader, ID3D11ShaderResourceView* inSrv,
                 ID3D11UnorderedAccessView* outUav, const Params& params,
                 uint32_t numGroupsX, uint32_t numGroupsY) {
        ID3D11DeviceContext* dctx = ctx.Get();
        dctx->UpdateSubresource(cb.Get(), 0, nullptr, &params, 0, 0);
        dctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
        dctx->CSSetShaderResources(0, 1, &inSrv);
        dctx->CSSetUnorderedAccessViews(0, 1, &outUav, nullptr);
        dctx->CSSetShader(shader, nullptr, 0);
        dctx->Dispatch(numGroupsX, numGroupsY, 1);

        ID3D11UnorderedAccessView* nullUav = nullptr;
        ID3D11ShaderResourceView* nullSrv = nullptr;
        dctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        dctx->CSSetShaderResources(0, 1, &nullSrv);
    }

    // Proves the device can actually execute a compute dispatch and read back
    // the result. Some virtualized hosts hand out a "hardware" device (e.g. the
    // Basic Render Driver) whose dispatches return garbage or raise an access
    // violation; tryCreate() rejects such devices so the pipeline falls back to
    // CPU. Blurring a constant field must reproduce it exactly (the normalized
    // kernel sums to 1), so any deviation -- including NaN -- fails the probe.
    // SEH is legal here because this function holds only POD locals.
    bool probeValidity() {
        constexpr UINT kProbeW = 16;
        constexpr UINT kProbeH = 16;
        constexpr size_t kProbeN = kProbeW * kProbeH;
        const size_t bytes = kProbeN * sizeof(float);
        if (!ensureBuffers(bytes)) {
            return false;
        }
        Params params;
        params.w = kProbeW;
        params.h = kProbeH;
        params.radius = 3;  // exact path for sigma=1: radius = ceil(3*1)
        params.mode = 0;
        float sum = 0.0f;
        for (int i = -3; i <= 3; ++i) {
            const float v = std::exp(-0.5f * static_cast<float>(i * i));
            params.kernel[static_cast<size_t>(i + 3)] = v;
            sum += v;
        }
        for (int i = 0; i <= 6; ++i) {
            params.kernel[static_cast<size_t>(i)] /= sum;
        }

        __try {
            float field[kProbeN];
            std::fill(field, field + kProbeN, 1.0f);
            ctx->UpdateSubresource(bufA.Get(), 0, nullptr, field, 0, 0);
            runPass(shaderH.Get(), srvA.Get(), uavB.Get(), params, 1, 1);
            runPass(shaderV.Get(), srvB.Get(), uavA.Get(), params, 1, 1);
            ctx->CopyResource(staging.Get(), bufA.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                return false;
            }
            const float* p = static_cast<const float*>(mapped.pData);
            bool ok = true;
            for (size_t i = 0; i < kProbeN; ++i) {
                const float v = p[i];
                if (!(v >= 0.99f && v <= 1.01f)) {  // also rejects NaN
                    ok = false;
                    break;
                }
            }
            ctx->Unmap(staging.Get(), 0);
            return ok;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // D3D11 immediate context is not thread-safe; serialize every blur().
    mutable std::mutex mtx;
};

D3D11SpatialFilterBackend::~D3D11SpatialFilterBackend() = default;

std::shared_ptr<const SpatialFilterBackend>
D3D11SpatialFilterBackend::tryCreate(bool allowWarp) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    bool warp = false;

    HRESULT hr = createDeviceSeh(D3D_DRIVER_TYPE_HARDWARE, true, &device, &context);
    if (FAILED(hr) && allowWarp) {
        hr = createDeviceSeh(D3D_DRIVER_TYPE_WARP, true, &device, &context);
        warp = SUCCEEDED(hr);
    }
    if (FAILED(hr)) {
        return nullptr;  // no usable device -> caller falls back to CPU
    }

    auto impl = std::make_unique<Impl>();
    impl->device = std::move(device);
    impl->ctx = std::move(context);
    impl->warp = warp;

    impl->shaderH = compileShader(impl->device.Get(), "BlurH");
    impl->shaderV = compileShader(impl->device.Get(), "BlurV");
    if (!impl->shaderH || !impl->shaderV) {
        return nullptr;  // shader compile failure -> CPU fallback
    }

    // Constant buffer (288 bytes, 16-byte aligned).
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(Params);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(impl->device->CreateBuffer(&cbDesc, nullptr, &impl->cb))) {
        return nullptr;
    }

    // Compute-validation probe: reject devices that cannot actually execute a
    // dispatch (some virtualized hosts return a usable-looking device that
    // produces garbage or crashes on the first real work).
    if (!impl->probeValidity()) {
        return nullptr;
    }

    auto backend = std::shared_ptr<D3D11SpatialFilterBackend>(
        new D3D11SpatialFilterBackend());
    backend->impl_ = std::move(impl);
    return backend;
}

const char* D3D11SpatialFilterBackend::name() const noexcept {
    return impl_ && impl_->warp ? "d3d11-warp" : "d3d11";
}

void D3D11SpatialFilterBackend::blur(const float* src, int w, int h, float sigma,
                                     std::vector<float>& out) const {
    std::lock_guard<std::mutex> lock(impl_->mtx);

    // Guard degenerate dimensions before any size arithmetic: a non-positive
    // width or height must yield an empty output, never a huge bad_alloc from
    // a sign-extended size_t.
    if (w <= 0 || h <= 0) {
        out.clear();
        return;
    }
    out.resize(static_cast<size_t>(w) * h);
    const size_t n = static_cast<size_t>(w) * h;
    if (sigma <= 1e-4f) {
        std::memcpy(out.data(), src, n * sizeof(float));
        return;
    }

    // Mirror the CPU kernel selection (Pipeline.cpp gaussianBlur).
    Params params;
    params.w = static_cast<uint32_t>(w);
    params.h = static_cast<uint32_t>(h);
    int boxPasses = 0;  // exact path: 1 H/V pair; box path: 3 H/V pairs
    if (sigma <= 4.0f) {
        const int radius = std::max(1, static_cast<int>(std::ceil(sigma * 3.0f)));
        params.radius = static_cast<uint32_t>(radius);
        params.mode = 0;
        float sum = 0.0f;
        for (int i = -radius; i <= radius; ++i) {
            const float v =
                std::exp(-0.5f * (static_cast<float>(i) / sigma) *
                         (static_cast<float>(i) / sigma));
            params.kernel[static_cast<size_t>(i + radius)] = v;
            sum += v;
        }
        for (int i = 0; i <= radius * 2; ++i) {
            params.kernel[static_cast<size_t>(i)] /= sum;
        }
        boxPasses = 1;
    } else {
        const float wf = std::sqrt(12.0f * sigma * sigma / 3.0f + 1.0f);
        int wBox = static_cast<int>(std::floor(wf));
        if ((wBox & 1) == 0) {
            wBox += 1;
        }
        wBox = std::max(wBox, 1);
        params.radius = static_cast<uint32_t>((wBox - 1) / 2);
        params.invBox = 1.0f / static_cast<float>(wBox);
        params.mode = 1;
        boxPasses = 3;
    }

    const size_t bytes = n * sizeof(float);
    if (!impl_->ensureBuffers(bytes)) {
        // Allocation failure: degrade deterministically to a zeroed output
        // rather than leaving uninitialized floats.
        std::fill(out.begin(), out.end(), 0.0f);
        return;
    }

    // Dispatch geometry: flattened 1D over w*h threads; 2D grid rows of
    // kThreadsPerRow threads keep the dispatch within D3D11 group limits.
    const uint64_t totalGroups = (static_cast<uint64_t>(n) + 255u) / 256u;
    const uint32_t numGroupsX =
        static_cast<uint32_t>(std::min<uint64_t>(totalGroups, 65535u));
    const uint32_t numGroupsY =
        static_cast<uint32_t>((totalGroups + 65534u) / 65535u);

    ID3D11DeviceContext* ctx = impl_->ctx.Get();
    // Upload source into bufA; ping-pong: A -> B -> A -> ... final in A.
    ctx->UpdateSubresource(impl_->bufA.Get(), 0, nullptr, src, 0, 0);
    for (int pass = 0; pass < boxPasses; ++pass) {
        impl_->runPass(impl_->shaderH.Get(), impl_->srvA.Get(),
                       impl_->uavB.Get(), params, numGroupsX, numGroupsY);
        impl_->runPass(impl_->shaderV.Get(), impl_->srvB.Get(),
                       impl_->uavA.Get(), params, numGroupsX, numGroupsY);
    }

    // Synchronous readback: staging Map forces completion of every queued
    // command, so `out` is fully written before blur() returns.
    ctx->CopyResource(impl_->staging.Get(), impl_->bufA.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx->Map(impl_->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        std::memcpy(out.data(), mapped.pData, n * sizeof(float));
        ctx->Unmap(impl_->staging.Get(), 0);
    } else {
        std::fill(out.begin(), out.end(), 0.0f);
    }
}

}  // namespace lrc
