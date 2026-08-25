// Standalone reproduction harness for the intermittent SIGSEGV in the pooled
// gaussian blur test. Copies ONLY the blur path from Pipeline.cpp (boxBlur*,
// gaussianBlur) + Parallel.cpp + RenderScratch.h. Installs a vectored exception
// handler that prints a symbolized stack trace on access violation.
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <iostream>
#include <vector>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <algorithm>

#include "utils/Parallel.h"
#include "engine/RenderScratch.h"

#pragma comment(lib, "dbghelp.lib")

namespace lrc {
inline float smoothstep01(float x) {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

void boxBlurHorizontal(const float* src, float* dst, int w, int h,
                       int radius, float invW) {
    const size_t rowGrain = lrc::parallel::defaultGrain(static_cast<size_t>(h));
    lrc::parallel::parallelFor(static_cast<size_t>(h), rowGrain,
                               [&](size_t by, size_t ey) {
        for (int y = static_cast<int>(by); y < static_cast<int>(ey); ++y) {
            const float* srow = src + static_cast<size_t>(y) * w;
            float* drow = dst + static_cast<size_t>(y) * w;
            float sum = 0.0f;
            for (int i = -radius; i <= radius; ++i)
                sum += srow[std::clamp(i, 0, w - 1)];
            drow[0] = sum * invW;
            for (int x = 1; x < w; ++x) {
                sum += srow[std::clamp(x + radius, 0, w - 1)] -
                       srow[std::clamp(x - 1 - radius, 0, w - 1)];
                drow[x] = sum * invW;
            }
        }
    });
}

void boxBlurVertical(const float* src, float* dst, int w, int h,
                     int radius, float invW) {
    const size_t colGrain = lrc::parallel::defaultGrain(static_cast<size_t>(w));
    lrc::parallel::parallelFor(static_cast<size_t>(w), colGrain,
                               [&](size_t bx, size_t ex) {
        const int x0 = static_cast<int>(bx);
        const int x1 = static_cast<int>(ex);
        const size_t ncols = static_cast<size_t>(x1 - x0);
        ScratchBuffer sumsBuf(ncols);
        float* sums = sumsBuf.data();
        std::fill(sums, sums + ncols, 0.0f);
        for (int i = -radius; i <= radius; ++i) {
            const int sy = std::clamp(i, 0, h - 1);
            const float* srow = src + static_cast<size_t>(sy) * w;
            for (int x = x0; x < x1; ++x)
                sums[static_cast<size_t>(x - x0)] += srow[x];
        }
        float* drow0 = dst;
        for (int x = x0; x < x1; ++x)
            drow0[x] = sums[static_cast<size_t>(x - x0)] * invW;
        for (int y = 1; y < h; ++y) {
            const float* rowAdd =
                src + static_cast<size_t>(std::clamp(y + radius, 0, h - 1)) * w;
            const float* rowSub =
                src + static_cast<size_t>(std::clamp(y - 1 - radius, 0, h - 1)) * w;
            float* drow = dst + static_cast<size_t>(y) * w;
            for (int x = x0; x < x1; ++x) {
                const size_t c = static_cast<size_t>(x - x0);
                sums[c] += rowAdd[x] - rowSub[x];
                drow[x] = sums[c] * invW;
            }
        }
    });
}

void gaussianBlur(const float* src, int w, int h, float sigma,
                  std::vector<float>& out) {
    out.resize(static_cast<size_t>(w) * h);
    if (w <= 0 || h <= 0)
        return;
    if (sigma <= 1e-4f) {
        std::memcpy(out.data(), src, out.size() * sizeof(float));
        return;
    }
    if (sigma <= 4.0f) {
        const int radius = std::max(1, static_cast<int>(std::ceil(sigma * 3.0f)));
        std::array<float, 32> kernel;
        float sum = 0.0f;
        for (int i = -radius; i <= radius; ++i) {
            const float v = std::exp(-0.5f * (static_cast<float>(i) / sigma) *
                                     (static_cast<float>(i) / sigma));
            kernel[static_cast<size_t>(i + radius)] = v;
            sum += v;
        }
        for (int i = 0; i <= radius * 2; ++i)
            kernel[static_cast<size_t>(i)] /= sum;

        ScratchBuffer tmp(out.size());
        const size_t rowGrain = lrc::parallel::defaultGrain(static_cast<size_t>(h));
        lrc::parallel::parallelFor(static_cast<size_t>(h), rowGrain,
                                   [&](size_t by, size_t ey) {
            for (int y = static_cast<int>(by); y < static_cast<int>(ey); ++y) {
                for (int x = 0; x < w; ++x) {
                    float acc = 0.0f;
                    for (int i = -radius; i <= radius; ++i) {
                        const int sx = std::clamp(x + i, 0, w - 1);
                        acc += src[static_cast<size_t>(y) * w + sx] *
                               kernel[static_cast<size_t>(i + radius)];
                    }
                    tmp[static_cast<size_t>(y) * w + x] = acc;
                }
            }
        });
        lrc::parallel::parallelFor(static_cast<size_t>(h), rowGrain,
                                   [&](size_t by, size_t ey) {
            for (int y = static_cast<int>(by); y < static_cast<int>(ey); ++y) {
                for (int x = 0; x < w; ++x) {
                    float acc = 0.0f;
                    for (int i = -radius; i <= radius; ++i) {
                        const int sy = std::clamp(y + i, 0, h - 1);
                        acc += tmp[static_cast<size_t>(sy) * w + x] *
                               kernel[static_cast<size_t>(i + radius)];
                    }
                    out[static_cast<size_t>(y) * w + x] = acc;
                }
            }
        });
        return;
    }

    const float wf = std::sqrt(12.0f * sigma * sigma / 3.0f + 1.0f);
    int wBox = static_cast<int>(std::floor(wf));
    if ((wBox & 1) == 0)
        wBox += 1;
    wBox = std::max(wBox, 1);
    const int radius = (wBox - 1) / 2;
    const float invW = 1.0f / static_cast<float>(wBox);

    ScratchBuffer tmpA(out.size());
    boxBlurHorizontal(src, tmpA.data(), w, h, radius, invW);
    boxBlurVertical(tmpA.data(), out.data(), w, h, radius, invW);
    boxBlurHorizontal(out.data(), tmpA.data(), w, h, radius, invW);
    boxBlurVertical(tmpA.data(), out.data(), w, h, radius, invW);
    boxBlurHorizontal(out.data(), tmpA.data(), w, h, radius, invW);
    boxBlurVertical(tmpA.data(), out.data(), w, h, radius, invW);
}

std::vector<float> pipelineGaussianBlurTest(const float* src, int w, int h,
                                            float sigma) {
    std::vector<float> out;
    gaussianBlur(src, w, h, sigma, out);
    return out;
}
} // namespace lrc

// --- Crash handling ------------------------------------------------------
static void printStack(HANDLE hProcess, DWORD64 base, CONTEXT* ctx) {
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = ctx->Rip;
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx->Rbp;
    sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx->Rsp;
    sf.AddrStack.Mode = AddrModeFlat;

    std::cout << "\n===== CRASH STACK (frames) =====\n";
    for (int i = 0; i < 40; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProcess, GetCurrentThread(),
                         &sf, ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr))
            break;
        DWORD64 off = 0;
        std::string name = "<unknown>";
        alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        if (SymFromAddr(hProcess, sf.AddrPC.Offset, &off, sym)) {
            name = sym->Name;
        }
        char mod[MAX_PATH] = {};
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(sf.AddrPC.Offset), &hm)) {
            GetModuleFileNameA(hm, mod, MAX_PATH);
        }
        std::cout << "  #" << i << " " << name << " +0x" << std::hex << off
                  << std::dec << "  [" << mod << " @ 0x" << std::hex
                  << (sf.AddrPC.Offset - base) << std::dec << "]\n";
    }
}

static LONG WINAPI vehHandler(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode == 0xC0000005 /* access violation */) {
        std::cout << "\n!!!! SIGSEGV / access violation at address 0x"
                  << std::hex << ep->ExceptionRecord->ExceptionInformation[1]
                  << std::dec << " (code " << std::hex
                  << ep->ExceptionRecord->ExceptionCode << std::dec << ")\n";
        HANDLE proc = GetCurrentProcess();
        DWORD64 base = 0;
        SymInitialize(proc, nullptr, TRUE);
        printStack(proc, base, ep->ContextRecord);
        fflush(stdout);
        // Terminate rather than continuing to run corrupted state.
        ExitProcess(99);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

using namespace lrc;

int main() {
    AddVectoredExceptionHandler(1, &vehHandler);

    std::cout << "Harness: warm up workers...\n";
    // Mimic prior pipeline tests: spin the pool with small parallelFor calls.
    for (int i = 0; i < 50; ++i) {
        lrc::parallel::parallelFor(8, 4, [&](size_t b, size_t e) {
            volatile size_t acc = 0;
            for (size_t j = b; j < e; ++j) acc += j;
            (void)acc;
        });
    }
    std::cout << "Harness: workers warmed, running blur loop...\n";

    const int w = 320, h = 240;
    std::vector<float> grad(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            grad[static_cast<size_t>(y) * w + x] =
                static_cast<float>(x + y) / static_cast<float>(w + h - 2);

    const float sigmas[] = {2.5f, static_cast<float>(std::min(w, h)) / 50.0f};

    long long iterations = 0;
    for (int iter = 0; iter < 20000; ++iter) {
        for (const float sigma : sigmas) {
            std::vector<float> golden =
                lrc::pipelineGaussianBlurTest(grad.data(), w, h, sigma);
            for (int rep = 0; rep < 6; ++rep) {
                ScratchBuffer big(static_cast<size_t>(w) * h * 2);
                std::fill(big.data(), big.data() + big.size(), 0.25f);
                std::vector<float> again =
                    lrc::pipelineGaussianBlurTest(grad.data(), w, h, sigma);
                if (again != golden) {
                    std::cout << "MISMATCH at iter " << iter << " sigma " << sigma
                              << " rep " << rep << "\n";
                    return 2;
                }
                ++iterations;
            }
        }
    }
    std::cout << "Completed " << iterations << " blur renders, no crash.\n";
    return 0;
}
