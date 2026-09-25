#include "engine/PregradeAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/AiInference.h"
#include "engine/AiMaskGenerator.h"
#include "engine/Adjustments.h"

namespace lrc {
namespace {

// Rec.709 luma in [0,1] from linear RGB (matches the engine's pipeline helper).
inline float lumaOf(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

} // namespace

PregradeResult PregradeAnalyzer::pregrade(const ImageBuffer& linear,
                                          AiInferenceEngine* engine) {
    PregradeResult out;
    if (linear.empty())
        return out;

    // Normalize to F32 RGB interleaved so all formats share one code path.
    ImageBufferPtr f32 = (linear.format() == PixelFormat::F32)
                             ? std::make_shared<ImageBuffer>(linear)
                             : linear.toF32();
    const float* d = reinterpret_cast<const float*>(f32->data().data());
    const int W = f32->width();
    const int H = f32->height();
    const size_t n = static_cast<size_t>(W) * H;
    if (n == 0)
        return out;
    out.processed = true;

    // ---- tonal statistics (mean / p5-p95 span) for suggestAuto + score -----
    int hist[256] = {};
    for (size_t i = 0; i < n; ++i)
        hist[std::clamp(static_cast<int>(lumaOf(d[i * 3], d[i * 3 + 1], d[i * 3 + 2]) * 255.0f), 0, 255)]++;
    double meanLuma = 0.0;
    for (int bin = 0; bin < 256; ++bin)
        meanLuma += hist[bin] * (bin / 255.0);
    meanLuma /= static_cast<double>(n);

    const size_t p5n = std::max<size_t>(1, static_cast<size_t>(0.05 * n));
    const size_t p95n = std::max<size_t>(1, static_cast<size_t>(0.95 * n));
    size_t acc = 0;
    int lo = 0;
    for (int bin = 0; bin < 256; ++bin) { acc += hist[bin]; if (acc >= p5n) { lo = bin; break; } }
    acc = 0;
    int hi = 255;
    for (int bin = 0; bin < 256; ++bin) { acc += hist[bin]; if (acc >= p95n) { hi = bin; break; } }
    const double span = (hi - lo) / 255.0;

    // suggestAuto: mirror B-1's exposure / contrast triggers.
    const double desiredExp = std::clamp(std::log2(0.18 / std::max(meanLuma, 1e-6)), -3.0, 3.0);
    const double desiredCont = std::clamp((0.45 - span) * 140.0, -50.0, 50.0);
    out.suggestAuto = std::abs(desiredExp) >= 0.05 || std::abs(desiredCont) >= 2.0;

    // ---- downsampled interest grid: edge energy + centre saliency ----------
    // Bounded to keep analysis cheap regardless of the source resolution; the
    // caller usually feeds a preview-size buffer but we stay safe anyway.
    constexpr int kGrid = 160;
    const int gw = std::max(2, std::min(kGrid, W));
    const int gh = std::max(2, std::min(kGrid, H));
    const size_t gridN = static_cast<size_t>(gw) * gh;
    std::vector<float> interest(gridN, 0.0f);
    double gradSum = 0.0;
    double centerSum = 0.0;
    size_t centerCnt = 0;
    for (int cy = 0; cy < gh; ++cy) {
        const int y = (H <= 1) ? 0 : std::min(H - 1, static_cast<int>((cy + 0.5f) * static_cast<float>(H) / gh));
        for (int cx = 0; cx < gw; ++cx) {
            const int x = (W <= 1) ? 0 : std::min(W - 1, static_cast<int>((cx + 0.5f) * static_cast<float>(W) / gw));
            const int xm = std::max(0, x - 1), xp = std::min(W - 1, x + 1);
            const int ym = std::max(0, y - 1), yp = std::min(H - 1, y + 1);
            const auto lum = [&](int px, int py) {
                const size_t pi = static_cast<size_t>(py) * W + px;
                return lumaOf(d[pi * 3], d[pi * 3 + 1], d[pi * 3 + 2]);
            };
            const float gx = lum(xp, y) - lum(xm, y);
            const float gy = lum(x, yp) - lum(x, ym);
            const float g = std::abs(gx) + std::abs(gy);
            interest[static_cast<size_t>(cy) * gw + cx] = g;
            gradSum += g;
            // Central 50% band = a likely subject zone.
            const float nx = static_cast<float>(cx) / gw;
            const float ny = static_cast<float>(cy) / gh;
            if (nx >= 0.25f && nx <= 0.75f && ny >= 0.25f && ny <= 0.75f) {
                centerSum += g;
                ++centerCnt;
            }
        }
    }
    const double meanGrad = gradSum / static_cast<double>(gridN);
    const double centerMean = (centerCnt > 0) ? centerSum / static_cast<double>(centerCnt) : meanGrad;

    // ---- quality score (sharpness + tonal range), deterministic -------------
    const double edgeScore = std::min(100.0, meanGrad * 200.0);
    const double rangeScore = std::min(100.0, span * 100.0);
    out.score = static_cast<int>(std::round(0.5 * edgeScore + 0.5 * rangeScore));

    // ---- hasSubject --------------------------------------------------------
    // Preferred path: rmbg2-subject segmentation when the model is loaded.
    if (engine != nullptr && engine->hasModel("rmbg2-subject")) {
        try {
            AiMaskGenerator gen(engine);
            AiMaskParams params;
            params.target = AiMaskTarget::Subject;
            const ImageBufferPtr mask = gen.generate(linear, params, "rmbg2-subject");
            if (mask != nullptr && !mask->empty()) {
                // AiMaskGenerator returns an RGB buffer with R=G=B = weight, so
                // each pixel's weight is the first of its 3 equal channels.
                const float* m = reinterpret_cast<const float*>(mask->data().data());
                const size_t mn = static_cast<size_t>(mask->width()) * mask->height();
                double fg = 0.0;
                for (size_t i = 0; i < mn; ++i)
                    fg += m[i * 3];
                // Foreground fraction in [0,1]; a distinct subject occupies a
                // meaningful but not the entire frame.
                const double frac = fg / static_cast<double>(mn);
                out.hasSubject = frac > 0.12 && frac < 0.95;
                return out;
            }
        } catch (...) {
            // Segmentation threw (resource failure); fall through to statistics.
        }
    }
    // Deterministic fallback: a strong central saliency relative to the whole
    // frame suggests a central subject. Conservative threshold avoids flagging
    // uniform / empty shots.
    out.hasSubject = centerMean > 1e-4f && centerMean > meanGrad * 1.15;

    return out;
}

} // namespace lrc
