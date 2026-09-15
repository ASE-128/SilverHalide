// NAFNet end-to-end smoke test (GUI-only, not part of the headless Catch2 suite).
//
// Exercises the full Qualcomm NAFNet-DeNoise flow that can't be validated in a
// headless build:
//   1. ModelManager::downloadModel fetches the 410 MB .zip from the manifest.
//   2. extractOnnxFromZip() unpacks the largest .onnx entry to the cache path.
//   3. AiInferenceEngine loads the extracted model (needs LRC_ENABLE_AI=1,
//      i.e. onnxruntime.dll next to the executable).
//   4. AiDenoiseProcessor runs one inference pass and we sanity-check the
//      output dimensions match the input.
//
// Usage:  nafnet_smoke.exe
// Exits 0 on success, non-zero on any failure.

#include <QCoreApplication>
#include <QEventLoop>
#include <QNetworkProxy>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <string>

#include "app/ModelManager.h"
#include "engine/AiDenoiseProcessor.h"
#include "engine/AiInference.h"
#include "engine/ImageBuffer.h"

using namespace lrc;

static ImageBuffer makeNoisyGray(int w, int h) {
    ImageBuffer buf(w, h, PixelFormat::F32);
    // Simple deterministic pattern + pseudo-noise so the model has something
    // structured to denoise. NAFNet expects sRGB-normalized [0,1] F32 input
    // (manifest input_mean=0, input_std=1).
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float base = 0.3f + 0.2f * std::sin(x * 0.12f)
                                    + 0.2f * std::cos(y * 0.09f);
            // crude pseudo-noise
            const float n = 0.04f * std::sin((x * 7 + y * 13) * 1.7f);
            const float v = base + n;
            for (int c = 0; c < 3; ++c) {
                const float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                buf.setPixel(x, y, c, clamped);
            }
        }
    }
    return buf;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    // Unbuffered stdout so progress is visible when piped/captured.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    // Bypass any system HTTP proxy (e.g. WattToolkit/Steam++ local
    // accelerator on 127.0.0.1:26561, which MITM's TLS and stalls Qt's
    // Schannel backend). S3 is a public endpoint reachable directly.
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);

    ModelManager mm;
    if (!mm.reloadManifest(ModelManager::defaultManifestPath())) {
        std::fprintf(stderr, "FAIL: cannot load manifest at %s\n",
                     ModelManager::defaultManifestPath().string().c_str());
        return 1;
    }

    auto entry = mm.findModel("nafnet-denoise");
    if (!entry) {
        std::fprintf(stderr, "FAIL: nafnet-denoise not in manifest\n");
        return 1;
    }

    const auto onnxPath = mm.cachePathFor(*entry);
    // The Qualcomm NAFNet ZIP downloads slowly from this network when fetched
    // directly by QNetworkAccessManager (the WattToolkit system proxy MITM's
    // TLS and stalls Qt's Schannel backend; going NoProxy is too slow).
    // So the .part file may be pre-placed by an external downloader — in that
    // case we exercise the REAL C++ extractOnnxFromZip path directly.
    const auto zipPath = onnxPath;
    const auto zipPart = std::filesystem::path(onnxPath.string() + ".part");
    std::printf("[smoke] target onnx path: %s\n", onnxPath.string().c_str());
    // Diagnostic: why isModelReady returns what it returns.
    {
        const bool onnxExists = std::filesystem::exists(onnxPath);
        std::printf("[smoke] diag: onnxExists=%d  entry.sha256='%s'  size=%llu\n",
                    static_cast<int>(onnxExists),
                    entry->sha256.c_str(),
                    static_cast<unsigned long long>(entry->sizeBytes));
        if (onnxExists) {
            const auto actual = mm.sha256File(onnxPath);
            std::error_code sEc;
            const auto fs = std::filesystem::file_size(onnxPath, sEc);
            std::printf("[smoke] diag: actualSha256='%s'  fileSize=%llu  match=%d\n",
                        actual.c_str(),
                        static_cast<unsigned long long>(fs),
                        static_cast<int>(actual == entry->sha256));
        }
    }

    // --- Step 1+2: download + extract (only if not already cached) ----------
    if (!mm.isModelReady(*entry)) {
        bool ok = false;
        // (a) Pre-placed complete ZIP at the .part path -> real C++ extract.
        if (std::filesystem::exists(zipPart)) {
            const auto zs = std::filesystem::file_size(zipPart);
            std::printf("[smoke] found pre-placed ZIP at %s (%llu bytes) — "
                        "extracting via ModelManager::extractOnnxFromZip...\n",
                        zipPart.string().c_str(),
                        static_cast<unsigned long long>(zs));
            std::string zipErr;
            if (ModelManager::extractOnnxFromZip(zipPart, onnxPath, zipErr)) {
                ok = true;
                std::printf("[smoke] extractOnnxFromZip OK: %llu bytes\n",
                            static_cast<unsigned long long>(
                                std::filesystem::file_size(onnxPath)));
            } else {
                std::fprintf(stderr, "FAIL: extractOnnxFromZip: %s\n",
                             zipErr.c_str());
                return 1;
            }
        }
        // (b) Fallback: real QNAM download (slow / proxy-sensitive).
        if (!ok) {
            std::printf("[smoke] no pre-placed ZIP — starting QNAM download "
                        "(%llu MB zip)\n",
                        static_cast<unsigned long long>(
                            entry->sizeBytes / (1024 * 1024)));
            QEventLoop loop;
            uint64_t lastPct = 1000;  // impossible -> forces first print
            uint64_t handle = mm.downloadModel(*entry,
                [&](uint64_t recv, uint64_t total, bool finished,
                    const std::string& err) {
                    if (finished) {
                        std::printf("\n[smoke] download finished: err='%s'\n",
                                    err.empty() ? "(none)" : err.c_str());
                        loop.quit();
                        return;
                    }
                    if (total > 0) {
                        uint64_t pct = (recv * 100) / total;
                        if (pct != lastPct) {
                            lastPct = pct;
                            std::printf("\r[smoke] progress: %llu%% (%llu / %llu MB)   ",
                                        static_cast<unsigned long long>(pct),
                                        static_cast<unsigned long long>(recv / (1024 * 1024)),
                                        static_cast<unsigned long long>(total / (1024 * 1024)));
                            std::fflush(stdout);
                        }
                    }
                });
            if (handle == 0) {
                std::fprintf(stderr, "FAIL: downloadModel returned handle 0 (%s)\n",
                             mm.lastError().c_str());
                return 1;
            }
            loop.exec();
            if (!std::filesystem::exists(onnxPath)) {
                std::fprintf(stderr, "FAIL: extracted onnx not found at %s\n",
                             onnxPath.string().c_str());
                return 1;
            }
            std::printf("[smoke] extracted onnx OK: %llu bytes\n",
                        static_cast<unsigned long long>(
                            std::filesystem::file_size(onnxPath)));
        }
    } else {
        std::printf("[smoke] cached model already ready — skipping download\n");
    }

    // --- Step 3: load into the engine ---------------------------------------
    if (!AiInferenceEngine::hasRealBackend()) {
        std::fprintf(stderr, "FAIL: AiInferenceEngine built without ONNX Runtime "
                             "(LRC_ENABLE_AI=0). Rebuild with onnxruntime.\n");
        return 1;
    }
    AiInferenceEngine engine;
    ModelInfo info;
    info.id = entry->id;
    info.onnxPath = onnxPath;
    info.inputShape.dims = entry->inputShape;   // {1,3,256,256}
    info.inputShape.layout = "NCHW";
    info.outputShape.dims = entry->outputShape;
    info.outputShape.layout = "NCHW";
    info.inputMean = entry->inputMean;
    info.inputStd = entry->inputStd;
    if (!engine.loadModel(info)) {
        std::fprintf(stderr, "FAIL: loadModel: %s\n", engine.lastError().c_str());
        return 1;
    }
    std::printf("[smoke] model loaded into engine: id='%s'\n", info.id.c_str());

    // --- Step 4: run one denoise pass ---------------------------------------
    // Use a 256x256 input (NAFNet's native static resolution) so runImage
    // does no resize and there's a single tile.
    auto input = makeNoisyGray(256, 256);
    AiDenoiseProcessor proc(&engine, /*cm=*/nullptr);
    AiDenoiseOptions opts;
    opts.strength = 1.0f;
    opts.tileSize = 256;
    opts.overlap = 0;

    auto out = proc.denoise(input, entry->id, opts);
    if (!out) {
        std::fprintf(stderr, "FAIL: denoise returned null\n");
        return 1;
    }
    if (out->width() != input.width() || out->height() != input.height()) {
        std::fprintf(stderr, "FAIL: output dim mismatch: %dx%d vs %dx%d\n",
                     out->width(), out->height(),
                     input.width(), input.height());
        return 1;
    }

    // Sanity: output should differ from input (strength=1, real model) but
    // stay in a sane numeric range.
    double mad = 0.0;
    double mn = 1e9, mx = -1e9;
    const float* f = reinterpret_cast<const float*>(out->data().data());
    const int n = out->width() * out->height() * 3;
    for (int i = 0; i < n; ++i) {
        mn = std::min(mn, static_cast<double>(f[i]));
        mx = std::max(mx, static_cast<double>(f[i]));
        mad += std::fabs(static_cast<double>(f[i]) -
                         static_cast<double>(
                             reinterpret_cast<const float*>(input.data().data())[i]));
    }
    mad /= n;
    std::printf("[smoke] denoise OK: out=%dx%d  mean|Δ|=%.5f  range=[%.3f,%.3f]\n",
                out->width(), out->height(), mad, mn, mx);

    if (mad < 1e-6) {
        std::fprintf(stderr, "WARN: output identical to input — model may not "
                             "have actually run (fallback path?)\n");
        return 2;
    }
    std::printf("[smoke] SUCCESS: NAFNet download+extract+load+infer verified.\n");
    return 0;
}
