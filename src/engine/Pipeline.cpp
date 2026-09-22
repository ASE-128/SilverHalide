#include "engine/Pipeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "engine/ColorManager.h"
#include "engine/LensfunProfile.h"
#include "engine/RenderScratch.h"
#include "engine/SpatialFilter.h"
#include "engine/ZoneCurve.h"
#include "utils/MathUtils.h"
#include "utils/Parallel.h"
#include "app/Constants.h"

// AI inference wiring. These headers are intentionally guarded by
// LRC_ENABLE_AI in CMake — when AI is disabled, the fallback bilateral NR /
// heuristic mask paths run instead (see AiDenoiseProcessor / AiMaskGenerator
// for the contract). When AI is enabled but Pipeline::globalAiEngine() is null
// (headless tests, pre-MainWindow init), the same fallbacks run.
#ifdef LRC_ENABLE_AI
#  include "engine/AiDenoiseProcessor.h"
#  include "engine/AiInference.h"
#  include "engine/AiMaskGenerator.h"
#endif

// M4-3 optional SIMD batches. Defined by CMake only when the SIMD path is
// wanted; the default build keeps the original scalar loops untouched.
#ifdef LRC_ENABLE_SIMD
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <xmmintrin.h>
#include <emmintrin.h>
#endif
#if defined(__AVX2__)
#include <immintrin.h>
#endif
#endif

namespace lrc {

namespace {

// Hermite smoothstep: returns a value smoothly transitioning from 0 for x <= 0
// to 1 for x >= 1 with zero first derivatives at both endpoints.
inline float smoothstep01(float x) {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

// Monotone piecewise-cubic interpolation of the tone curve points. Returns a
// 4096 entry LUT mapping normalized linear input to normalized linear output.
// Shares the interpolation with the CurveEditor widget so the curve the user
// drags renders exactly as previewed.
std::array<float, 4096> buildCurveLut(
    const std::vector<ToneCurvePoint>& points) {
    std::array<float, 4096> lut{};
    for (size_t idx = 0; idx < 4096; ++idx) {
        const float t = static_cast<float>(idx) / 4095.0f;
        lut[idx] = std::clamp(zonecurve::interpolate(points, t), 0.0f, 1.0f);
    }
    return lut;
}

inline float curveLookup(const std::array<float, 4096>& lut, float t) {
    const size_t idx = static_cast<size_t>(std::clamp(t, 0.0f, 1.0f) * 4095.0f);
    return lut[idx];
}

// --- HSL per-channel helpers ------------------------------------------------

// Width of each colour range (half-width in degrees). Beyond this range the
// weight drops to zero. Adjacent ranges overlap so transitions are smooth.
// Centre hue angles come from the shared lrc::kColorRangeCenters.
constexpr float kRangeHalfWidth = 40.0f;

// Converts RGB to a hue angle in degrees [0, 360), saturation [0, 1], and
// luminance (Rec.709 luma).
inline void rgbToHSL(float r, float g, float b,
                     float& hue, float& sat, float& luma) {
    luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    const float mn = std::min({r, g, b});
    const float mx = std::max({r, g, b});
    const float range = mx - mn;
    sat = (mx > 1e-6f) ? range / mx : 0.0f;

    if (range < 1e-6f) {
        hue = 0.0f;
        return;
    }
    float h = 0.0f;
    if (mx == r)
        h = 60.0f * (g - b) / range;
    else if (mx == g)
        h = 60.0f * (b - r) / range + 120.0f;
    else
        h = 60.0f * (r - g) / range + 240.0f;
    if (h < 0.0f) h += 360.0f;
    hue = h;
}

// Returns a weight [0,1] for how much a given hue belongs to a colour range.
inline float hueWeight(float hueDeg, int rangeIdx) {
    float d = std::abs(hueDeg - lrc::kColorRangeCenters[rangeIdx]);
    if (d > 180.0f) d = 360.0f - d;
    if (d >= kRangeHalfWidth) return 0.0f;
    return smoothstep01(1.0f - d / kRangeHalfWidth);
}

// Hue/sat/luma distance -> smooth band weight. dn in [0,1]; tol in [0,1];
// softness in [0,1]. Softens the band edge (a colour mask feather).
static float rangeAxisWeight(float dn, double tol, double softness) {
    const float band = 0.05f + static_cast<float>(tol) * 0.95f;
    const float soft = 0.05f + static_cast<float>(softness) * 0.45f;
    const float edge = band * (1.0f - soft);
    return 1.0f - smoothstep01((dn - edge) / std::max(band * soft, 1e-4f));
}

// Reconstructs RGB from the pixel's HSL after applying a hue shift (degrees),
// a multiplicative saturation scale, and a multiplicative luma scale. Uses the
// standard HSL->RGB formulas so a hue rotation produces the expected colour
// (e.g. red +30° -> a visible yellow instead of a dimmed red).
// The pixel's hue/saturation/luma and channel extrema are precomputed by the
// caller (a single rgbToHSL), so this helper does not re-derive them.
inline void applyHSLReconstruct(float& r, float& g, float& b,
                                float hue, float sat, float luma,
                                float mx, float mn,
                                float hueShiftDeg, float satScale,
                                float lumaScale) {
    (void)sat;  // not used: the reconstruction needs HSL saturation, not rgbToHSL's
    (void)luma; // not used: the reconstruction needs HSL lightness, not Rec.709 luma.

    // Apply hue shift (wrapping around 360°).
    float newHue = std::fmod(hue + hueShiftDeg, 360.0f);
    if (newHue < 0.0f) newHue += 360.0f;

    // Derive HSL lightness + saturation from the pixel's channel extrema.
    const float L = 0.5f * (mx + mn);
    float S = 0.0f;
    if (mx - mn > 1e-6f && L > 1e-6f && L < 1.0f - 1e-6f)
        S = std::clamp((mx - mn) / (1.0f - std::abs(2.0f * L - 1.0f)), 0.0f, 1.0f);

    // Apply saturation and luminance adjustments.
    const float newSat = std::clamp(S * satScale, 0.0f, 1.0f);
    const float newL = std::clamp(L * lumaScale, 0.0f, 1.0f);

    // Reconstruct via standard HSL->RGB.
    const float C = (1.0f - std::abs(2.0f * newL - 1.0f)) * newSat;
    const float Hp = newHue / 60.0f;
    const float X = C * (1.0f - std::abs(std::fmod(Hp, 2.0f) - 1.0f));

    float r1, g1, b1;
    if (Hp < 1.0f) { r1 = C; g1 = X; b1 = 0.0f; }
    else if (Hp < 2.0f) { r1 = X; g1 = C; b1 = 0.0f; }
    else if (Hp < 3.0f) { r1 = 0.0f; g1 = C; b1 = X; }
    else if (Hp < 4.0f) { r1 = 0.0f; g1 = X; b1 = C; }
    else if (Hp < 5.0f) { r1 = X; g1 = 0.0f; b1 = C; }
    else { r1 = C; g1 = 0.0f; b1 = X; }

    const float m = newL - C * 0.5f;
    r = r1 + m;
    g = g1 + m;
    b = b1 + m;
}

// Applies the per-channel HSL adjustments to a single RGB pixel in place.
// Hue shifts blend across the 8 colour ranges by hue proximity; saturation and
// luminance are scaled the same way.
inline void applyHSLPerChannel(float& r, float& g, float& b,
                                const std::array<HSLChannel, 8>& hsl,
                                uint8_t activeMask) {
    float hue, sat, luma;
    rgbToHSL(r, g, b, hue, sat, luma);
    const float mx = std::max({r, g, b});
    const float mn = std::min({r, g, b});

    // Accumulate blended adjustments from the active colour ranges weighted by
    // hue proximity. Channels whose hue/saturation/luminance are all zero are
    // skipped via activeMask.
    float hueShift = 0.0f;
    float satScale = 0.0f;
    float lumaScale = 0.0f;
    float totalWeight = 0.0f;

    for (int ci = 0; ci < 8; ++ci) {
        if ((activeMask & (1u << ci)) == 0u) continue;
        const float w = hueWeight(hue, ci);
        if (w < 1e-6f) continue;
        const auto& ch = hsl[ci];
        hueShift += static_cast<float>(ch.hue) * w;
        satScale += (1.0f + static_cast<float>(ch.saturation) / 100.0f) * w;
        lumaScale += (1.0f + static_cast<float>(ch.luminance) / 100.0f) * w;
        totalWeight += w;
    }

    if (totalWeight < 1e-6f)
        return;

    hueShift /= totalWeight;
    satScale /= totalWeight;
    lumaScale /= totalWeight;
    applyHSLReconstruct(r, g, b, hue, sat, luma, mx, mn,
                        hueShift, satScale, lumaScale);
}

// --- Point-colour targets ----------------------------------------------------

// Pixel-independent data for a colour target, precomputed once before the
// per-pixel loop. The reference colour is converted to HSL a single time per
// target instead of once per pixel.
struct ColorTargetPrecomputed {
    float refHue;
    float refSat;
    float refLuma;
    float hueShift;   // degrees
    float satScale;   // multiplicative (1.0 + saturation/100)
    float lumaScale;  // multiplicative (1.0 + luminance/100)
    float range;      // 0..100
    float smoothness; // 0..100
    float amount;     // 0..100
};

// Hoists every target's reference-colour HSL conversion and parameter scaling
// out of the per-pixel loop. Disabled / no-effect targets are dropped.
std::vector<ColorTargetPrecomputed> precomputeColorTargets(
    const std::vector<ColorTarget>& targets) {
    std::vector<ColorTargetPrecomputed> out;
    out.reserve(targets.size());
    for (const ColorTarget& ct : targets) {
        if (!ct.enabled || !ct.hasEffect())
            continue;
        ColorTargetPrecomputed p;
        rgbToHSL(ct.refR, ct.refG, ct.refB, p.refHue, p.refSat, p.refLuma);
        p.hueShift = static_cast<float>(ct.hueShift);
        p.satScale = 1.0f + static_cast<float>(ct.saturation) / 100.0f;
        p.lumaScale = 1.0f + static_cast<float>(ct.luminance) / 100.0f;
        p.range = static_cast<float>(ct.range);
        p.smoothness = static_cast<float>(ct.smoothness);
        p.amount = static_cast<float>(ct.amount);
        out.push_back(p);
    }
    return out;
}

// Applies a colour target to a single RGB pixel. The pixel is adjusted only to
// the extent that its hue/saturation match the target's reference colour;
// `range`/`smoothness` shape the band and `amount` scales the overall effect.
// The pixel's HSL/extrema are computed once here and reused by the
// reconstruction (no second rgbToHSL per pixel per target).
inline void applyColorTarget(float& r, float& g, float& b,
                             const ColorTargetPrecomputed& p) {
    float hue, sat, luma;
    rgbToHSL(r, g, b, hue, sat, luma);
    const float mx = std::max({r, g, b});
    const float mn = std::min({r, g, b});

    // Hue distance normalized to [0,1] (1 == 180° away).
    float dh = std::abs(hue - p.refHue);
    if (dh > 180.0f) dh = 360.0f - dh;
    dh /= 180.0f;

    // Band width: range 0 -> ~0.15 (very selective), 100 -> ~1.0 (whole wheel).
    const float band = 0.15f + p.range / 100.0f * 0.85f;
    const float soft = 0.05f + p.smoothness / 100.0f * 0.5f;
    const float edge = band * (1.0f - soft);
    const float hw = 1.0f - smoothstep01((dh - edge) /
                                         std::max(band * soft, 1e-4f));

    // Saturation closeness: pixels far from the reference saturation fade out.
    const float ds = std::abs(sat - p.refSat);
    const float sw = 1.0f - smoothstep01((ds - 0.25f) / 0.25f);

    const float weight = hw * sw * p.amount / 100.0f;
    if (weight <= 1e-4f)
        return;

    const float origR = r, origG = g, origB = b;
    applyHSLReconstruct(r, g, b, hue, sat, luma, mx, mn,
                        p.hueShift, p.satScale, p.lumaScale);
    r = origR + (r - origR) * weight;
    g = origG + (g - origG) * weight;
    b = origB + (b - origB) * weight;
}

// --- Split toning helpers ----------------------------------------------------

// Applies split toning to a single RGB pixel.
// Highlights are tinted with highlightHue/highlightSaturation,
// shadows with shadowHue/shadowSaturation. The balance slider shifts the
// transition midpoint.
inline void applySplitTone(float& r, float& g, float& b,
                           const SplitTone& st) {
    if (st.highlightSaturation < 0.5 && st.shadowSaturation < 0.5)
        return; // negligible effect; skip computation

    // Compute luminance (linear light, Rec.709 luma).
    const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;

    // Balance shifts the midpoint: -100 shifts toward shadows (more highlight
    // tone), +100 shifts toward highlights (more shadow tone).
    // At balance = 0, midpoint = 0.5 (in linear light).
    const float balanceNorm = static_cast<float>(st.balance) / 100.0f;
    // Balance in LR: negative = more highlight tone applied (transition moves
    // left, so more mid-tones take the highlight colour), positive = more
    // shadow tone (transition moves right).
    const float midpoint = 0.5f + balanceNorm * 0.3f;
    const float transition = 0.3f;

    float hw = 0.0f, sw = 0.0f;
    if (luma <= midpoint - transition * 0.5f) {
        hw = 0.0f; sw = 1.0f;
    } else if (luma >= midpoint + transition * 0.5f) {
        hw = 1.0f; sw = 0.0f;
    } else {
        const float t = (luma - (midpoint - transition * 0.5f)) / transition;
        hw = smoothstep01(t);
        sw = 1.0f - hw;
    }

    // Normalize weights so they sum to 1.
    const float totalW = hw + sw;
    if (totalW < 1e-6f) return;
    hw /= totalW;
    sw /= totalW;

    // Convert highlight and shadow hues to RGB tint colours.
    auto hueToRGB = [](float hueDeg) -> std::array<float, 3> {
        const float h = std::fmod(hueDeg, 360.0f);
        const float hNorm = h / 60.0f;
        const int sector = static_cast<int>(hNorm) % 6;
        const float frac = hNorm - static_cast<float>(sector);
        const float lx = 1.0f - frac;
        switch (sector) {
            case 0: return {1.0f, frac, 0.0f};
            case 1: return {lx, 1.0f, 0.0f};
            case 2: return {0.0f, 1.0f, frac};
            case 3: return {0.0f, lx, 1.0f};
            case 4: return {frac, 0.0f, 1.0f};
            default: return {1.0f, 0.0f, lx};
        }
    };

    const auto hCol = hueToRGB(st.highlightHue);
    const auto sCol = hueToRGB(st.shadowHue);

    const float hSat = static_cast<float>(st.highlightSaturation) / 100.0f;
    const float sSat = static_cast<float>(st.shadowSaturation) / 100.0f;

    // Blend: result = (1 - sat) * luma + sat * tint_colour, then mix.
    // This gives a colour that ranges from neutral to the pure tint.
    const float hR = (1.0f - hSat) * luma + hSat * hCol[0];
    const float hG = (1.0f - hSat) * luma + hSat * hCol[1];
    const float hB = (1.0f - hSat) * luma + hSat * hCol[2];

    const float sR = (1.0f - sSat) * luma + sSat * sCol[0];
    const float sG = (1.0f - sSat) * luma + sSat * sCol[1];
    const float sB = (1.0f - sSat) * luma + sSat * sCol[2];

    r = hw * hR + sw * sR;
    g = hw * hG + sw * sG;
    b = hw * hB + sw * sB;
}

// --- Spatial helpers (texture / clarity / sharpening / lens) ----------------

inline float rec709Luma(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// One horizontal box-blur pass over a single-channel image. Each output pixel
// is the mean of the radius-wide window in its row, sampled with edge clamping;
// a sliding sum makes the pass O(1) per pixel, independent of sigma.
void boxBlurHorizontal(const float* src, float* dst, int w, int h,
                       int radius, float invW) {
    const size_t rowGrain =
        lrc::parallel::defaultGrain(static_cast<size_t>(h));
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

// One vertical box-blur pass over a single-channel image. Each column block
// slides its own running sums down the image; blocks split over columns so
// reads stay cache-friendly per row.
void boxBlurVertical(const float* src, float* dst, int w, int h,
                     int radius, float invW) {
    const size_t colGrain =
        lrc::parallel::defaultGrain(static_cast<size_t>(w));
    lrc::parallel::parallelFor(static_cast<size_t>(w), colGrain,
                               [&](size_t bx, size_t ex) {
        const int x0 = static_cast<int>(bx);
        const int x1 = static_cast<int>(ex);
        const size_t ncols = static_cast<size_t>(x1 - x0);
        // Running sums come from the thread-local scratch pool (reused across
        // blocks/calls); they must be zeroed before accumulation.
        ScratchBuffer sumsBuf(ncols);
        float* sums = sumsBuf.data();
        std::fill(sums, sums + ncols, 0.0f);
        // Initial window (y = 0): clamped to the top edge.
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
                src +
                static_cast<size_t>(std::clamp(y - 1 - radius, 0, h - 1)) * w;
            float* drow = dst + static_cast<size_t>(y) * w;
            for (int x = x0; x < x1; ++x) {
                const size_t c = static_cast<size_t>(x - x0);
                sums[c] += rowAdd[x] - rowSub[x];
                drow[x] = sums[c] * invW;
            }
        }
    });
}

#if defined(LRC_ENABLE_SIMD) && defined(__AVX2__)
// P2-2: AVX2 fast path for the exact separable blur. Each output row is
// independent; within a row the 8-lane block keeps per-pixel summation order
// identical to the scalar kernel (each lane is its own accumulator); the
// fused-multiply-add reassociates, so output is tolerance-identical rather than
// bit-identical (see P2-2 acceptance: Δ<0.008). Edge columns fall back to scalar.
//
// `vertical` selects which coordinate varies per tap:
//   horizontal: src[y*w + x+i]  -> 8 contiguous columns at row y.
//   vertical:   src[(y+i)*w+x]  -> 8 contiguous columns at clamped row y+i.
void blurExactPassAVX2(const float* src, float* dst, int w, int h, int radius,
                       const float* kernel, bool vertical) {
    // Interior columns fully inside [radius, w-radius) can be vectorized.
    const int interior = w - 2 * radius;
    const int blocks = interior / 8;                 // full 8-lane groups
    const int vecStart = radius;
    const int vecEnd = vecStart + blocks * 8;

    lrc::parallel::parallelFor(static_cast<size_t>(h),
                               lrc::parallel::defaultGrain(static_cast<size_t>(h)),
                               [&](size_t by, size_t ey) {
        for (int y = static_cast<int>(by); y < static_cast<int>(ey); ++y) {
            float* drow = dst + static_cast<size_t>(y) * w;
            // Scalar edges needing clamp: [0, vecStart) and [vecEnd, w).
            for (int x = 0; x < vecStart; ++x) {
                float acc = 0.0f;
                if (vertical) {
                    for (int i = -radius; i <= radius; ++i) {
                        const int sy = std::clamp(y + i, 0, h - 1);
                        acc += src[static_cast<size_t>(sy) * w + x] *
                               kernel[static_cast<size_t>(i + radius)];
                    }
                } else {
                    for (int i = -radius; i <= radius; ++i) {
                        const int sx = std::clamp(x + i, 0, w - 1);
                        acc += src[static_cast<size_t>(y) * w + sx] *
                               kernel[static_cast<size_t>(i + radius)];
                    }
                }
                drow[x] = acc;
            }
            for (int x = vecEnd; x < w; ++x) {
                float acc = 0.0f;
                if (vertical) {
                    for (int i = -radius; i <= radius; ++i) {
                        const int sy = std::clamp(y + i, 0, h - 1);
                        acc += src[static_cast<size_t>(sy) * w + x] *
                               kernel[static_cast<size_t>(i + radius)];
                    }
                } else {
                    for (int i = -radius; i <= radius; ++i) {
                        const int sx = std::clamp(x + i, 0, w - 1);
                        acc += src[static_cast<size_t>(y) * w + sx] *
                               kernel[static_cast<size_t>(i + radius)];
                    }
                }
                drow[x] = acc;
            }
            // AVX2 interior.
            for (int xb = vecStart; xb < vecEnd; xb += 8) {
                __m256 acc = _mm256_setzero_ps();
                for (int i = -radius; i <= radius; ++i) {
                    const __m256 k = _mm256_set1_ps(
                        kernel[static_cast<size_t>(i + radius)]);
                    const float* p = vertical
                        ? src + (static_cast<size_t>(std::clamp(y + i, 0, h - 1)) * w + xb)
                        : src + (static_cast<size_t>(y) * w + xb + i);
                    acc = _mm256_fmadd_ps(_mm256_loadu_ps(p), k, acc);
                }
                _mm256_storeu_ps(drow + xb, acc);
            }
        }
    });
}
#endif // LRC_ENABLE_SIMD && __AVX2__

// Separable Gaussian blur of a single-channel image. sigma <= 0 returns a copy.
// Small sigma (<= 4.0) uses the exact O(radius) separable convolution; large
// sigma (> 4.0, e.g. clarity at ~min(w,h)/50) uses three O(1)-per-pixel
// sliding-window box passes whose cascade approximates the Gaussian. Both paths
// clamp sample coordinates to the image edge. The result is written into the
// caller-provided `out` buffer; the internal temporaries come from the
// thread-local scratch pool so the render hot path performs no heap allocation.
void gaussianBlur(const float* src, int w, int h, float sigma,
                  std::vector<float>& out) {
    out.resize(static_cast<size_t>(w) * h);
    if (w <= 0 || h <= 0)
        return;
    if (sigma <= 1e-4f) {
        std::memcpy(out.data(), src, out.size() * sizeof(float));
        return;
    }

    // --- Exact separable convolution for small sigma (bit-for-bit stable). ---
    if (sigma <= 4.0f) {
        const int radius = std::max(1, static_cast<int>(std::ceil(sigma * 3.0f)));
        // The kernel is tiny (<= 25 taps for sigma <= 4); a fixed stack array
        // avoids a per-call heap allocation.
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

        #if defined(LRC_ENABLE_SIMD) && defined(__AVX2__)
        // P2-2 AVX2 fast path. blurExactPassAVX2 runs the exact separable
        // convolution with 8-lane FMA accumulation; each lane keeps the same
        // per-pixel summation order as the scalar kernel, so the output is
        // tolerance-identical (FMA reassociation yields Δ ~1e-7, far under the
        // P2-2 acceptance Δ<0.008) rather than bit-identical. Edge columns use
        // the identical scalar clamp path.
        {
            ScratchBuffer tmp(out.size());
            blurExactPassAVX2(src, tmp.data(), w, h, radius, kernel.data(),
                              false /* horizontal */);
            blurExactPassAVX2(tmp.data(), out.data(), w, h, radius,
                              kernel.data(), true /* vertical */);
        }
        return;
#else
        // Horizontal pass (each output row is independent).
        ScratchBuffer tmp(out.size());
        const size_t rowGrain =
            lrc::parallel::defaultGrain(static_cast<size_t>(h));
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
        // Vertical pass.
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
#endif
    }

    // --- Fast path: 3 identical box-blur passes approximate the Gaussian. ---
    // One box of width wBox has variance (wBox^2 - 1)/12; n=3 passes give
    // sigma^2 = 3 * (wBox^2 - 1)/12, so wBox = sqrt(12*sigma^2/3 + 1).
    const float wf = std::sqrt(12.0f * sigma * sigma / 3.0f + 1.0f);
    int wBox = static_cast<int>(std::floor(wf));
    if ((wBox & 1) == 0)
        wBox += 1; // round to the nearest odd width (ties round up)
    wBox = std::max(wBox, 1);
    const int radius = (wBox - 1) / 2;
    const float invW = 1.0f / static_cast<float>(wBox);

    // A single scratch buffer alternates with the output buffer: every pass
    // reads a fully-written previous stage and the output is fully overwritten
    // each time it is used as an intermediate, so the cascade is bit-identical
    // to the original two-scratch-buffer version.
    ScratchBuffer tmpA(out.size());

    boxBlurHorizontal(src, tmpA.data(), w, h, radius, invW);
    boxBlurVertical(tmpA.data(), out.data(), w, h, radius, invW);
    boxBlurHorizontal(out.data(), tmpA.data(), w, h, radius, invW);
    boxBlurVertical(tmpA.data(), out.data(), w, h, radius, invW);
    boxBlurHorizontal(out.data(), tmpA.data(), w, h, radius, invW);
    boxBlurVertical(tmpA.data(), out.data(), w, h, radius, invW);
}

// Computes the Rec.709 luminance of an RGB-interleaved F32 buffer into the
// caller-provided `out` buffer (no per-call allocation).
void lumaImage(const float* p, int w, int h, std::vector<float>& out) {
    const size_t n = static_cast<size_t>(w) * h;
    out.resize(n);
    lrc::parallel::parallelFor(n, lrc::parallel::defaultGrain(n),
                               [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i)
            out[i] = rec709Luma(p[i * 3], p[i * 3 + 1], p[i * 3 + 2]);
    });
}

// Bilinear sample of an RGB F32 buffer at (sx, sy) in pixel coordinates.
inline void sampleRGB(const float* p, int w, int h, float sx, float sy,
                      float& r, float& g, float& b) {
    const float x0 = std::floor(sx), y0 = std::floor(sy);
    const float fx = sx - x0, fy = sy - y0;
    const int x1 = static_cast<int>(x0), y1 = static_cast<int>(y0);
    const int x2 = std::clamp(x1 + 1, 0, w - 1), y2 = std::clamp(y1 + 1, 0, h - 1);
    const int xi = std::clamp(x1, 0, w - 1), yi = std::clamp(y1, 0, h - 1);
    auto at = [&](int x, int y, int c) {
        return p[(static_cast<size_t>(y) * w + x) * 3 + c];
    };
    for (int c = 0; c < 3; ++c) {
        const float v00 = at(xi, yi, c), v10 = at(x2, yi, c);
        const float v01 = at(xi, y2, c), v11 = at(x2, y2, c);
        const float top = v00 + (v10 - v00) * fx;
        const float bot = v01 + (v11 - v01) * fx;
        const float v = top + (bot - top) * fy;
        if (c == 0) r = v; else if (c == 1) g = v; else b = v;
    }
}

// --- Single-channel downsample / upsample helpers for the clarity blur --------
//
// Clarity's blur radius derives from min(w,h)/50, which at full resolution is a
// very large sigma (a 1920px frame -> ~38) whose three box passes would cover
// the whole frame. The blur therefore runs on a ~1/4-linear-size area-average
// downsample of the luma, with the sigma scaled by the downsample ratio, and
// the blurred field is bilinearly upsampled back to full resolution.

// Single-channel bilinear sample at (sx, sy) in pixel coordinates, edge clamped.
// Mirrors the RGB sampler used by the lens-distortion remap above.
inline float sampleLuma(const float* p, int w, int h, float sx, float sy) {
    const float x0 = std::floor(sx), y0 = std::floor(sy);
    const float fx = sx - x0, fy = sy - y0;
    const int x1 = static_cast<int>(x0), y1 = static_cast<int>(y0);
    const int x2 = std::clamp(x1 + 1, 0, w - 1);
    const int y2 = std::clamp(y1 + 1, 0, h - 1);
    const int xi = std::clamp(x1, 0, w - 1);
    const int yi = std::clamp(y1, 0, h - 1);
    const float v00 = p[static_cast<size_t>(yi) * w + xi];
    const float v10 = p[static_cast<size_t>(yi) * w + x2];
    const float v01 = p[static_cast<size_t>(y2) * w + xi];
    const float v11 = p[static_cast<size_t>(y2) * w + x2];
    const float top = v00 + (v10 - v00) * fx;
    const float bot = v01 + (v11 - v01) * fx;
    return top + (bot - top) * fy;
}

// Area-average downsample of a single-channel image into a dw*dh buffer (the
// caller precomputes dw/dh from the ~1/4-long-edge target and the aspect
// ratio). Each output pixel is the mean of its corresponding source block; the
// block partition exactly covers every source pixel, so downsampling then
// upsampling preserves the mean. Parallelized over output rows.
void downsampleLumaArea(const float* src, int w, int h, int dw, int dh,
                        std::vector<float>& out) {
    out.resize(static_cast<size_t>(dw) * dh);
    if (dw <= 0 || dh <= 0)
        return;
    if (dw == w && dh == h) {
        std::memcpy(out.data(), src, out.size() * sizeof(float));
        return;
    }
    const size_t rowGrain =
        lrc::parallel::defaultGrain(static_cast<size_t>(dh));
    lrc::parallel::parallelFor(static_cast<size_t>(dh), rowGrain,
                               [&](size_t by, size_t ey) {
        for (int oy = static_cast<int>(by); oy < static_cast<int>(ey); ++oy) {
            const int sy0 = static_cast<int>(
                static_cast<size_t>(oy) * h / dh);
            int sy1 = static_cast<int>(
                static_cast<size_t>(oy + 1) * h / dh);
            sy1 = std::max(sy1, sy0 + 1);  // defensive: sy1 > sy0 always holds
            float* drow = out.data() + static_cast<size_t>(oy) * dw;
            for (int ox = 0; ox < dw; ++ox) {
                const int sx0 = static_cast<int>(
                    static_cast<size_t>(ox) * w / dw);
                int sx1 = static_cast<int>(
                    static_cast<size_t>(ox + 1) * w / dw);
                sx1 = std::max(sx1, sx0 + 1);  // defensive: sx1 > sx0 always holds
                float sum = 0.0f;
                for (int sy = sy0; sy < sy1; ++sy) {
                    const float* srow = src + static_cast<size_t>(sy) * w;
                    for (int sx = sx0; sx < sx1; ++sx)
                        sum += srow[sx];
                }
                drow[ox] = sum /
                    static_cast<float>((sx1 - sx0) * (sy1 - sy0));
            }
        }
    });
}

// Bilinear upsampling of a single-channel buffer from (sw,sh) back to (dw,dh),
// edge clamped, using the same pixel-coordinate mapping as sampleRGB/sampleLuma.
// Parallelized over output rows.
void upsampleLumaBilinear(const float* src, int sw, int sh, int dw, int dh,
                          std::vector<float>& out) {
    out.resize(static_cast<size_t>(dw) * dh);
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;
    if (dw == sw && dh == sh) {
        std::memcpy(out.data(), src, out.size() * sizeof(float));
        return;
    }
    const float sxScale =
        (dw > 1) ? static_cast<float>(sw - 1) / static_cast<float>(dw - 1) : 0.0f;
    const float syScale =
        (dh > 1) ? static_cast<float>(sh - 1) / static_cast<float>(dh - 1) : 0.0f;
    const size_t rowGrain =
        lrc::parallel::defaultGrain(static_cast<size_t>(dh));
    lrc::parallel::parallelFor(static_cast<size_t>(dh), rowGrain,
                               [&](size_t by, size_t ey) {
        for (int dy = static_cast<int>(by); dy < static_cast<int>(ey); ++dy) {
            const float sy =
                (dh > 1) ? static_cast<float>(dy) * syScale : 0.0f;
            float* drow = out.data() + static_cast<size_t>(dy) * dw;
            for (int dx = 0; dx < dw; ++dx) {
                const float sx =
                    (dw > 1) ? static_cast<float>(dx) * sxScale : 0.0f;
                drow[dx] = sampleLuma(src, sw, sh, sx, sy);
            }
        }
    });
}

// --- Film grain --------------------------------------------------------------

// Integer-lattice hash returning a pseudo-random value in [0,1).
inline float hashNoise(int x, int y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u +
                 static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFu) / static_cast<float>(0xFFFFF);
}

// Smooth value noise: bilinearly interpolated random lattice of the given cell
// size (in pixels). Larger cells -> coarser, smoother grain.
inline float grainValueNoise(float x, float y, float cellSize) {
    const float cx = x / std::max(cellSize, 1.0f);
    const float cy = y / std::max(cellSize, 1.0f);
    const int ix = static_cast<int>(std::floor(cx));
    const int iy = static_cast<int>(std::floor(cy));
    const float fx = smoothstep01(cx - std::floor(cx));
    const float fy = smoothstep01(cy - std::floor(cy));
    const float v00 = hashNoise(ix, iy);
    const float v10 = hashNoise(ix + 1, iy);
    const float v01 = hashNoise(ix, iy + 1);
    const float v11 = hashNoise(ix + 1, iy + 1);
    return v00 * (1.0f - fx) * (1.0f - fy) + v10 * fx * (1.0f - fy) +
           v01 * (1.0f - fx) * fy + v11 * fx * fy;
}

// --- Mask weight-field rasterization + global cache --------------------------

// Rasterizes `m`'s per-pixel weight into a w*h buffer. Each entry y*w+x is
// bit-identical to Pipeline::maskWeight(m, w, h, nx, ny) with the caller's
// sampling convention nx = x/(w-1), ny = y/(h-1) (both 0 when the dimension is
// 1). Brush masks are rasterized dab-by-dab over each dab's bounding box, so
// the cost is O(sum of dab areas) instead of O(pixels x dabs). Linear/radial
// masks use their closed-form O(1) formula per pixel, parallelized over rows.
std::vector<float> rasterizeMaskField(const Mask& m, int w, int h) {
    std::vector<float> field(static_cast<size_t>(w) * h, 0.0f);
    if (m.type == MaskType::Brush) {
        if (m.brush.empty()) {
            if (m.invert)
                std::fill(field.begin(), field.end(), 1.0f);
            return field;
        }
        const float minDim = static_cast<float>(std::min(w, h));
        for (const BrushDab& dab : m.brush) {
            // Soft-edge formula duplicated from Pipeline::maskWeight's Brush
            // branch (must stay bit-identical).
            const float rx = std::max(
                dab.radius * minDim / std::max(w, 1), 1e-4f);
            const float ry = std::max(
                dab.radius * minDim / std::max(h, 1), 1e-4f);
            const float soft = std::max(dab.softness, 1e-4f);

            // Bounding box of pixels whose normalized distance d < 1 (the
            // support of this dab's weight), padded by one pixel so the max is
            // taken over every pixel that maskWeight would touch.
            int x0 = 0, x1 = w, y0 = 0, y1 = h;
            if (w > 1) {
                const float px0 = (dab.x - rx) * static_cast<float>(w - 1);
                const float px1 = (dab.x + rx) * static_cast<float>(w - 1);
                x0 = std::clamp(static_cast<int>(std::floor(px0 - 1.0f)), 0, w);
                x1 = std::clamp(static_cast<int>(std::ceil(px1 + 1.0f)), 0, w);
            }
            if (h > 1) {
                const float py0 = (dab.y - ry) * static_cast<float>(h - 1);
                const float py1 = (dab.y + ry) * static_cast<float>(h - 1);
                y0 = std::clamp(static_cast<int>(std::floor(py0 - 1.0f)), 0, h);
                y1 = std::clamp(static_cast<int>(std::ceil(py1 + 1.0f)), 0, h);
            }

            for (int y = y0; y < y1; ++y) {
                const float ny =
                    (h > 1) ? static_cast<float>(y) / (h - 1) : 0.0f;
                const float dy = (ny - dab.y) / ry;
                float* row = field.data() + static_cast<size_t>(y) * w;
                for (int x = x0; x < x1; ++x) {
                    const float nx =
                        (w > 1) ? static_cast<float>(x) / (w - 1) : 0.0f;
                    const float dx = (nx - dab.x) / rx;
                    const float d = std::sqrt(dx * dx + dy * dy);
                    const float wi = 1.0f - smoothstep01(
                        (d - (1.0f - soft)) / soft);
                    if (wi > row[x])
                        row[x] = wi;  // per-dab max composition
                }
            }
        }
        if (m.invert) {
            for (float& v : field)
                v = 1.0f - v;
        }
        return field;
    }

    // Linear / radial / none: closed-form O(1) per pixel, rows are independent.
    const size_t rowGrain =
        lrc::parallel::defaultGrain(static_cast<size_t>(h));
    lrc::parallel::parallelFor(static_cast<size_t>(h), rowGrain,
                               [&](size_t by, size_t ey) {
        for (int y = static_cast<int>(by); y < static_cast<int>(ey); ++y) {
            const float ny =
                (h > 1) ? static_cast<float>(y) / (h - 1) : 0.0f;
            float* row = field.data() + static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x) {
                const float nx =
                    (w > 1) ? static_cast<float>(x) / (w - 1) : 0.0f;
                row[x] = Pipeline::maskWeight(m, w, h, nx, ny);
            }
        }
    });
    return field;
}

std::mutex g_weightMutex;
struct WeightEntry {
    size_t hash;
    int w;
    int h;
    std::shared_ptr<const std::vector<float>> field;
};
std::vector<WeightEntry> g_weightCache;
size_t g_weightCacheBytes = 0;
constexpr size_t kWeightCacheMaxBytes = 128u * 1024u * 1024u;

// FIFO eviction: entries are inserted at the back, so the oldest is the front.
// Called with g_weightMutex held. Bytes are estimated as sizeof(float)*w*h.
void evictWeightCacheLocked() {
    while (g_weightCacheBytes > kWeightCacheMaxBytes &&
           !g_weightCache.empty()) {
        const WeightEntry& e = g_weightCache.front();
        g_weightCacheBytes -=
            sizeof(float) * static_cast<size_t>(e.w) * static_cast<size_t>(e.h);
        g_weightCache.erase(g_weightCache.begin());
    }
}

// --- Post-crop vignette weight field + global cache -------------------------

// Mixes a double into a hash key (mirrors the Adjustments hash helpers).
inline void mixVignetteKey(size_t& h, double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    if (v == 0.0)
        bits = 0;
    h = hashCombine(h, bits);
    h = hashCombine(h, bits >> 32);
}

// Geometric post-crop vignette weight in [0,1] for the pixel (x,y) of a w*h
// buffer. The vignette is centred on the buffer's geometric centre
// ((w-1)/2, (h-1)/2) -- because the render buffer IS the post-crop image, the
// centre follows a future crop automatically with no extra state. roundness
// interpolates the distance between a rectangle (-100) and a circle (+100);
// midpoint places the falloff centre and feather sets its width. Only the
// Highlight-Priority shape is implemented (styles 0/1 are TODO).
float postCropVignetteWeight(const Adjustments& a, int w, int h, int x, int y) {
    const float cx = (w > 1) ? static_cast<float>(w - 1) * 0.5f : 0.0f;
    const float cy = (h > 1) ? static_cast<float>(h - 1) * 0.5f : 0.0f;
    const float nx = (cx > 0.0f) ? (static_cast<float>(x) - cx) / cx : 0.0f;
    const float ny = (cy > 0.0f) ? (static_cast<float>(y) - cy) / cy : 0.0f;

    // Roundness t in [0,1]: t=0 -> rectangle (max(|nx|,|ny|)), t=1 -> circle.
    const float t = static_cast<float>((a.vignetteRoundness + 100.0) / 200.0);
    const float ax = std::fabs(nx);
    const float ay = std::fabs(ny);
    const float dRect = std::max(ax, ay);
    // 1/sqrt(2) normalizes the corner (|nx|=|ny|=1) to distance 1.
    const float dCirc = std::sqrt(nx * nx + ny * ny) * 0.7071067811865476f;
    const float d = dRect + (dCirc - dRect) * t;  // in [0,1]

    // Midpoint m places the falloff centre; feather f sets its width. The
    // transition runs from m - f/2 (weight 0) to m + f/2 (weight 1).
    const float m = static_cast<float>(a.vignetteMidpoint) / 100.0f;  // [0,1]
    const float f = static_cast<float>(a.vignetteFeather) / 100.0f;   // [0,1]
    const float lo = m - f * 0.5f;
    const float hi = m + f * 0.5f;
    if (hi <= lo)
        return (d >= m) ? 1.0f : 0.0f;  // feather 0: hard step at the midpoint
    return smoothstep01((d - lo) / (hi - lo));
}

// Global FIFO cache for post-crop vignette weight fields. Keyed by
// (midpoint, roundness, feather, style, w, h) -- deliberately excluding amount
// and highlights, so dragging only the amount slider reuses the cached field.
std::mutex g_vignetteMutex;
struct VignetteEntry {
    size_t hash;
    int w;
    int h;
    std::shared_ptr<const std::vector<float>> field;
};
std::vector<VignetteEntry> g_vignetteCache;
size_t g_vignetteCacheBytes = 0;
constexpr size_t kVignetteCacheMaxBytes = 64u * 1024u * 1024u;

void evictVignetteCacheLocked() {
    while (g_vignetteCacheBytes > kVignetteCacheMaxBytes &&
           !g_vignetteCache.empty()) {
        const VignetteEntry& e = g_vignetteCache.front();
        g_vignetteCacheBytes -=
            sizeof(float) * static_cast<size_t>(e.w) * static_cast<size_t>(e.h);
        g_vignetteCache.erase(g_vignetteCache.begin());
    }
}

// Rasterizes the geometric weight for every pixel of a w*h buffer. Rows are
// independent, so the loop splits over rows (mirrors rasterizeMaskField).
std::vector<float> computeVignetteWeightField(const Adjustments& a, int w,
                                              int h) {
    std::vector<float> field(static_cast<size_t>(w) * h);
    const size_t rowGrain =
        lrc::parallel::defaultGrain(static_cast<size_t>(h));
    lrc::parallel::parallelFor(static_cast<size_t>(h), rowGrain,
                               [&](size_t by, size_t ey) {
        for (int y = static_cast<int>(by); y < static_cast<int>(ey); ++y) {
            float* row = field.data() + static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x)
                row[x] = postCropVignetteWeight(a, w, h, x, y);
        }
    });
    return field;
}

// Combines the enabled colour-range / luminance-range sub-mask weight fields
// over a w*h base image into a single [0,1] field. Each per-pixel
// band weight comes from Pipeline::colorSubMaskWeight / luminanceSubMaskWeight;
// when the modifier's smoothness is non-zero the field is softened with a
// spatial Gaussian (sigma grows with the slider). Enabled modifiers multiply
// (intersection) or max (union). Returns an empty vector when neither modifier
// is enabled (the caller then uses the geometry field alone).
std::vector<float> computeSubMaskField(const Mask& m, const float* base, int w,
                                       int h) {
    const size_t n = static_cast<size_t>(w) * h;
    std::vector<float> out;
    if (!m.useColorRange && !m.useLuminance)
        return out;
    // 0 is the identity of max (union), 1 of the product (intersection).
    out.assign(n, m.maskUnion ? 0.0f : 1.0f);
    const size_t grain = lrc::parallel::defaultGrain(n);

    if (m.useColorRange) {
        std::vector<float> cw(n);
        lrc::parallel::parallelFor(n, grain, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                cw[i] = Pipeline::colorSubMaskWeight(
                    m, base[i * 3], base[i * 3 + 1], base[i * 3 + 2]);
        });
        if (m.colorSmoothness > 0.5f) {
            const float sigma =
                0.5f + static_cast<float>(m.colorSmoothness) / 100.0f * 4.0f;
            gaussianBlur(cw.data(), w, h, sigma, cw);
        }
        if (m.maskUnion) {
            for (size_t i = 0; i < n; ++i)
                out[i] = std::max(out[i], cw[i]);
        } else {
            for (size_t i = 0; i < n; ++i)
                out[i] *= cw[i];
        }
    }

    if (m.useLuminance) {
        std::vector<float> lw(n);
        lrc::parallel::parallelFor(n, grain, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                lw[i] = Pipeline::luminanceSubMaskWeight(
                    m, base[i * 3], base[i * 3 + 1], base[i * 3 + 2]);
        });
        if (m.luminanceSmoothness > 0.5f) {
            const float sigma =
                0.5f + static_cast<float>(m.luminanceSmoothness) / 100.0f * 4.0f;
            gaussianBlur(lw.data(), w, h, sigma, lw);
        }
        if (m.maskUnion) {
            for (size_t i = 0; i < n; ++i)
                out[i] = std::max(out[i], lw[i]);
        } else {
            for (size_t i = 0; i < n; ++i)
                out[i] *= lw[i];
        }
    }
    return out;
}

// --- Per-pixel stage passes -------------------------------------------------
//
// The non-destructive stack is split into stages so the incremental
// render state can recompute only the stages affected by a parameter change.
// Each pass mutates the F32 buffer in place and is bit-identical to the
// corresponding section of the original single-loop applyAdjustments, so
// chaining A->B->C->D reproduces the old output exactly.

// --- Optional SIMD batches (LRC_ENABLE_SIMD) ----------------------------------
//
// The pure-linear stages of Stage A / Stage C are processed in 8-pixel batches
// (24 contiguous floats) with SSE2 intrinsics; the smoothstep-based stages stay
// on the original scalar code per pixel. Unaligned loads/stores mean no special
// alignment is required, and each parallelFor chunk is handled independently
// (8-pixel batches inside the chunk + a scalar tail for the remainder), so the
// SIMD selection never changes the chunking or the scalar output. With the macro
// undefined this file compiles to the untouched scalar path (bit-identical).

#ifdef LRC_ENABLE_SIMD

// Per-pixel remainder of Stage A after the vectorised head: highlights, shadows
// and whites/blacks (all smoothstep-weighted). Operates in place on r/g/b.
inline void stageA_pixelTail(float& r, float& g, float& b,
                             float highlightsGain, float shadowsGain,
                             float kHighlightsThresh, float kShadowsThresh,
                             bool useWB, float wGain, float bGain,
                             float kMid) {
    {
        const float hw = smoothstep01(
            (r - kHighlightsThresh) / (1.0f - kHighlightsThresh));
        const float hw_g = smoothstep01(
            (g - kHighlightsThresh) / (1.0f - kHighlightsThresh));
        const float hw_b = smoothstep01(
            (b - kHighlightsThresh) / (1.0f - kHighlightsThresh));
        r *= 1.0f + (highlightsGain - 1.0f) * hw;
        g *= 1.0f + (highlightsGain - 1.0f) * hw_g;
        b *= 1.0f + (highlightsGain - 1.0f) * hw_b;
    }
    {
        const float sw = smoothstep01((kShadowsThresh - r) / kShadowsThresh);
        const float sw_g =
            smoothstep01((kShadowsThresh - g) / kShadowsThresh);
        const float sw_b =
            smoothstep01((kShadowsThresh - b) / kShadowsThresh);
        r *= 1.0f + (shadowsGain - 1.0f) * sw;
        g *= 1.0f + (shadowsGain - 1.0f) * sw_g;
        b *= 1.0f + (shadowsGain - 1.0f) * sw_b;
    }
    if (useWB) {
        const float ww = smoothstep01((r - kMid) / (1.0f - kMid));
        const float ww_g = smoothstep01((g - kMid) / (1.0f - kMid));
        const float ww_b = smoothstep01((b - kMid) / (1.0f - kMid));
        r += (r - kMid) * wGain * ww;
        g += (g - kMid) * wGain * ww_g;
        b += (b - kMid) * wGain * ww_b;
        const float bw = smoothstep01((kMid - r) / kMid);
        const float bw_g = smoothstep01((kMid - g) / kMid);
        const float bw_b = smoothstep01((kMid - b) / kMid);
        r -= (kMid - r) * bGain * bw;
        g -= (kMid - g) * bGain * bw_g;
        b -= (kMid - b) * bGain * bw_b;
    }
}

// Stage A head in 8-pixel SIMD batches: exposure (v *= exposure) and contrast
// (v = kMid + (v - kMid) * cscale, clamp v >= 0). Both use the exact scalar op
// order, so the vector result is bit-identical; the smoothstep stages are then
// applied per pixel with stageA_pixelTail. Pixels that do not fill a full batch
// fall through to the untouched scalar body.
void applyStageABasicSIMD(float* p, size_t iBegin, size_t iEnd,
                          float exposure, float cscale, float kMid,
                          float highlightsGain, float shadowsGain,
                          float kHighlightsThresh, float kShadowsThresh,
                          bool useWB, float wGain, float bGain) {
    const __m128 expv = _mm_set1_ps(exposure);
    const __m128 kMidv = _mm_set1_ps(kMid);
    const __m128 cscalev = _mm_set1_ps(cscale);
    const __m128 zero = _mm_setzero_ps();

    size_t i = iBegin;
    const size_t iEndBatch = iBegin + ((iEnd - iBegin) / 8) * 8;
    for (; i < iEndBatch; i += 8) {
        // 8 pixels = 24 contiguous floats = 6 SSE lanes (unaligned).
        __m128 c0 = _mm_loadu_ps(p + i * 3 + 0);
        __m128 c1 = _mm_loadu_ps(p + i * 3 + 4);
        __m128 c2 = _mm_loadu_ps(p + i * 3 + 8);
        __m128 c3 = _mm_loadu_ps(p + i * 3 + 12);
        __m128 c4 = _mm_loadu_ps(p + i * 3 + 16);
        __m128 c5 = _mm_loadu_ps(p + i * 3 + 20);

        // Exposure.
        c0 = _mm_mul_ps(c0, expv);
        c1 = _mm_mul_ps(c1, expv);
        c2 = _mm_mul_ps(c2, expv);
        c3 = _mm_mul_ps(c3, expv);
        c4 = _mm_mul_ps(c4, expv);
        c5 = _mm_mul_ps(c5, expv);

        // Contrast + clamp: max(kMid + (v - kMid) * cscale, 0). Same op order
        // as the scalar body (sub, mul, add) so the arithmetic is bit-exact.
        c0 = _mm_max_ps(
            _mm_add_ps(_mm_mul_ps(_mm_sub_ps(c0, kMidv), cscalev), kMidv),
            zero);
        c1 = _mm_max_ps(
            _mm_add_ps(_mm_mul_ps(_mm_sub_ps(c1, kMidv), cscalev), kMidv),
            zero);
        c2 = _mm_max_ps(
            _mm_add_ps(_mm_mul_ps(_mm_sub_ps(c2, kMidv), cscalev), kMidv),
            zero);
        c3 = _mm_max_ps(
            _mm_add_ps(_mm_mul_ps(_mm_sub_ps(c3, kMidv), cscalev), kMidv),
            zero);
        c4 = _mm_max_ps(
            _mm_add_ps(_mm_mul_ps(_mm_sub_ps(c4, kMidv), cscalev), kMidv),
            zero);
        c5 = _mm_max_ps(
            _mm_add_ps(_mm_mul_ps(_mm_sub_ps(c5, kMidv), cscalev), kMidv),
            zero);

        _mm_storeu_ps(p + i * 3 + 0, c0);
        _mm_storeu_ps(p + i * 3 + 4, c1);
        _mm_storeu_ps(p + i * 3 + 8, c2);
        _mm_storeu_ps(p + i * 3 + 12, c3);
        _mm_storeu_ps(p + i * 3 + 16, c4);
        _mm_storeu_ps(p + i * 3 + 20, c5);

        // Smoothstep stages, scalar, per pixel (reads the vectorised head).
        for (size_t j = i; j < i + 8; ++j) {
            float r = p[j * 3 + 0];
            float g = p[j * 3 + 1];
            float b = p[j * 3 + 2];
            stageA_pixelTail(r, g, b, highlightsGain, shadowsGain,
                             kHighlightsThresh, kShadowsThresh, useWB, wGain,
                             bGain, kMid);
            p[j * 3 + 0] = r;
            p[j * 3 + 1] = g;
            p[j * 3 + 2] = b;
        }
    }
    // Scalar tail: pixels that did not fit a full 8-pixel batch use the exact
    // original scalar body (exposure -> contrast -> clamp -> smoothstep stages).
    for (; i < iEnd; ++i) {
        float r = p[i * 3 + 0];
        float g = p[i * 3 + 1];
        float b = p[i * 3 + 2];

        r *= exposure;
        g *= exposure;
        b *= exposure;

        r = kMid + (r - kMid) * cscale;
        g = kMid + (g - kMid) * cscale;
        b = kMid + (b - kMid) * cscale;
        if (r < 0.0f) r = 0.0f;
        if (g < 0.0f) g = 0.0f;
        if (b < 0.0f) b = 0.0f;

        stageA_pixelTail(r, g, b, highlightsGain, shadowsGain,
                         kHighlightsThresh, kShadowsThresh, useWB, wGain,
                         bGain, kMid);

        p[i * 3 + 0] = r;
        p[i * 3 + 1] = g;
        p[i * 3 + 2] = b;
    }
}

// Per-pixel remainder of Stage C after the vectorised WB head: dehaze and
// vibrance (kept scalar: vibrance needs per-pixel rgbToHSL, dehaze is a tiny
// affine branch). Operates in place on r/g/b.
inline void stageC_pixelTail(float& r, float& g, float& b,
                             float dhBoost, float kHazeFloor,
                             bool doDehaze, float vib) {
    if (doDehaze) {
        const float floor = kHazeFloor * (1.0f - dhBoost);
        r = floor + (r - floor) * dhBoost;
        g = floor + (g - floor) * dhBoost;
        b = floor + (b - floor) * dhBoost;
        r = std::max(r, 0.0f);
        g = std::max(g, 0.0f);
        b = std::max(b, 0.0f);
    }
    if (vib != 0.0f) {
        float hue, csat, luma;
        rgbToHSL(r, g, b, hue, csat, luma);
        float d = std::abs(hue - 42.0f);
        if (d > 180.0f) d = 360.0f - d;
        const float skinWeight = 1.0f - smoothstep01(d / 45.0f);
        const float boost =
            1.0f + vib * (1.0f - 0.75f * skinWeight) * (1.0f - csat);
        const float scale = csat > 1e-6f ? boost : 1.0f;
        r = luma + (r - luma) * scale;
        g = luma + (g - luma) * scale;
        b = luma + (b - luma) * scale;
    }
}

// Stage C head in 8-pixel SIMD batches: the white-balance multiply
// (r *= rTemp, g *= gTint, b *= bTemp) with per-channel constants laid out in
// the RGB-interleaved buffer. Dehaze and vibrance stay scalar per pixel.
void applyStageCBasicSIMD(float* p, size_t iBegin, size_t iEnd,
                          float rTemp, float gTint, float bTemp,
                          float dhBoost, float kHazeFloor,
                          bool doDehaze, float vib) {
    // 8 pixels = 24 floats; the (r,g,b) scaling pattern repeats every 3 lanes.
    const __m128 w0 = _mm_set_ps(rTemp, bTemp, gTint, rTemp); // floats 0-3
    const __m128 w1 = _mm_set_ps(gTint, rTemp, bTemp, gTint); // floats 4-7
    const __m128 w2 = _mm_set_ps(bTemp, gTint, rTemp, bTemp); // floats 8-11
    const __m128 w3 = w0;                                    // floats 12-15
    const __m128 w4 = w1;                                    // floats 16-19
    const __m128 w5 = w2;                                    // floats 20-23

    size_t i = iBegin;
    const size_t iEndBatch = iBegin + ((iEnd - iBegin) / 8) * 8;
    for (; i < iEndBatch; i += 8) {
        __m128 c0 = _mm_loadu_ps(p + i * 3 + 0);
        __m128 c1 = _mm_loadu_ps(p + i * 3 + 4);
        __m128 c2 = _mm_loadu_ps(p + i * 3 + 8);
        __m128 c3 = _mm_loadu_ps(p + i * 3 + 12);
        __m128 c4 = _mm_loadu_ps(p + i * 3 + 16);
        __m128 c5 = _mm_loadu_ps(p + i * 3 + 20);

        c0 = _mm_mul_ps(c0, w0);
        c1 = _mm_mul_ps(c1, w1);
        c2 = _mm_mul_ps(c2, w2);
        c3 = _mm_mul_ps(c3, w3);
        c4 = _mm_mul_ps(c4, w4);
        c5 = _mm_mul_ps(c5, w5);

        _mm_storeu_ps(p + i * 3 + 0, c0);
        _mm_storeu_ps(p + i * 3 + 4, c1);
        _mm_storeu_ps(p + i * 3 + 8, c2);
        _mm_storeu_ps(p + i * 3 + 12, c3);
        _mm_storeu_ps(p + i * 3 + 16, c4);
        _mm_storeu_ps(p + i * 3 + 20, c5);

        for (size_t j = i; j < i + 8; ++j) {
            float r = p[j * 3 + 0];
            float g = p[j * 3 + 1];
            float b = p[j * 3 + 2];
            stageC_pixelTail(r, g, b, dhBoost, kHazeFloor, doDehaze, vib);
            p[j * 3 + 0] = r;
            p[j * 3 + 1] = g;
            p[j * 3 + 2] = b;
        }
    }
    // Scalar tail: pixels that did not fit a full 8-pixel batch use the exact
    // original scalar body (WB -> dehaze -> vibrance).
    for (; i < iEnd; ++i) {
        float r = p[i * 3 + 0];
        float g = p[i * 3 + 1];
        float b = p[i * 3 + 2];

        r *= rTemp;
        b *= bTemp;
        g *= gTint;

        stageC_pixelTail(r, g, b, dhBoost, kHazeFloor, doDehaze, vib);

        p[i * 3 + 0] = r;
        p[i * 3 + 1] = g;
        p[i * 3 + 2] = b;
    }
}

#endif // LRC_ENABLE_SIMD

// Stage A: exposure -> contrast -> highlights -> shadows -> whites/blacks.
void applyStageA(const Adjustments& a, float* p, size_t pixelCount) {
    if (a.exposure == 0.0 && a.contrast == 0.0 && a.highlights == 0.0 &&
        a.shadows == 0.0 && a.whites == 0.0 && a.blacks == 0.0)
        return;
    const float exposure = static_cast<float>(std::pow(2.0, a.exposure));
    const float cscale = 1.0f + static_cast<float>(a.contrast) / 100.0f * 0.7f;
    constexpr float kMid = 0.18f;
    const float highlightsGain =
        1.0f + static_cast<float>(a.highlights) / 100.0f * 0.5f;
    const float shadowsGain =
        1.0f + static_cast<float>(a.shadows) / 100.0f * 0.5f;
    constexpr float kHighlightsThresh = 0.35f;
    constexpr float kShadowsThresh = 0.35f;
    const bool useWB = (a.whites != 0.0 || a.blacks != 0.0);
    const float wGain = static_cast<float>(a.whites) / 100.0f * 0.9f;
    const float bGain = static_cast<float>(a.blacks) / 100.0f * 0.9f;

#ifdef LRC_ENABLE_SIMD
    lrc::parallel::parallelFor(
        pixelCount, lrc::parallel::defaultGrain(pixelCount),
        [&](size_t iBegin, size_t iEnd) {
            applyStageABasicSIMD(p, iBegin, iEnd, exposure, cscale, kMid,
                                 highlightsGain, shadowsGain,
                                 kHighlightsThresh, kShadowsThresh,
                                 useWB, wGain, bGain);
        });
#else
    lrc::parallel::parallelFor(
        pixelCount, lrc::parallel::defaultGrain(pixelCount),
        [&](size_t iBegin, size_t iEnd) {
    for (size_t i = iBegin; i < iEnd; ++i) {
        float r = p[i * 3 + 0];
        float g = p[i * 3 + 1];
        float b = p[i * 3 + 2];

        // 1. Exposure (linear gain).
        r *= exposure;
        g *= exposure;
        b *= exposure;

        // 2. Contrast: linear gain around mid-gray (linear light).
        r = kMid + (r - kMid) * cscale;
        g = kMid + (g - kMid) * cscale;
        b = kMid + (b - kMid) * cscale;
        if (r < 0.0f) r = 0.0f;
        if (g < 0.0f) g = 0.0f;
        if (b < 0.0f) b = 0.0f;

        // 3. Highlights & shadows (smooth zone adjustment in linear light).
        {
            const float hw = smoothstep01(
                (r - kHighlightsThresh) / (1.0f - kHighlightsThresh));
            const float hw_g = smoothstep01(
                (g - kHighlightsThresh) / (1.0f - kHighlightsThresh));
            const float hw_b = smoothstep01(
                (b - kHighlightsThresh) / (1.0f - kHighlightsThresh));
            r *= 1.0f + (highlightsGain - 1.0f) * hw;
            g *= 1.0f + (highlightsGain - 1.0f) * hw_g;
            b *= 1.0f + (highlightsGain - 1.0f) * hw_b;
        }
        {
            const float sw =
                smoothstep01((kShadowsThresh - r) / kShadowsThresh);
            const float sw_g =
                smoothstep01((kShadowsThresh - g) / kShadowsThresh);
            const float sw_b =
                smoothstep01((kShadowsThresh - b) / kShadowsThresh);
            r *= 1.0f + (shadowsGain - 1.0f) * sw;
            g *= 1.0f + (shadowsGain - 1.0f) * sw_g;
            b *= 1.0f + (shadowsGain - 1.0f) * sw_b;
        }

        // 3.75 Whites & blacks: push the top/bottom of the tonal range.
        if (useWB) {
            const float ww = smoothstep01((r - kMid) / (1.0f - kMid));
            const float ww_g = smoothstep01((g - kMid) / (1.0f - kMid));
            const float ww_b = smoothstep01((b - kMid) / (1.0f - kMid));
            r += (r - kMid) * wGain * ww;
            g += (g - kMid) * wGain * ww_g;
            b += (b - kMid) * wGain * ww_b;
            const float bw = smoothstep01((kMid - r) / kMid);
            const float bw_g = smoothstep01((kMid - g) / kMid);
            const float bw_b = smoothstep01((kMid - b) / kMid);
            r -= (kMid - r) * bGain * bw;
            g -= (kMid - g) * bGain * bw_g;
            b -= (kMid - b) * bGain * bw_b;
        }

        p[i * 3 + 0] = r;
        p[i * 3 + 1] = g;
        p[i * 3 + 2] = b;
    }
    });
#endif
}

// Stage B: parametric tone curve (per-channel, linear space).
void applyStageB(const Adjustments& a, float* p, size_t pixelCount) {
    if (a.curve.size() < 2)
        return;
    const std::array<float, 4096> lut = buildCurveLut(a.curve);
    lrc::parallel::parallelFor(
        pixelCount, lrc::parallel::defaultGrain(pixelCount),
        [&](size_t iBegin, size_t iEnd) {
    for (size_t i = iBegin; i < iEnd; ++i) {
        p[i * 3 + 0] = curveLookup(lut, p[i * 3 + 0]);
        p[i * 3 + 1] = curveLookup(lut, p[i * 3 + 1]);
        p[i * 3 + 2] = curveLookup(lut, p[i * 3 + 2]);
    }
    });
}

// Stage C: white balance -> dehaze -> vibrance.
void applyStageC(const Adjustments& a, float* p, size_t pixelCount) {
    if (a.temperature == 0.0 && a.tint == 0.0 && a.dehaze == 0.0 &&
        a.vibrance == 0.0)
        return;
    const float rTemp =
        static_cast<float>(std::pow(2.0, a.temperature * 0.006));
    const float bTemp =
        static_cast<float>(std::pow(2.0, -a.temperature * 0.006));
    const float gTint = static_cast<float>(std::pow(2.0, a.tint * 0.006));
    const float dhBoost = 1.0f + static_cast<float>(a.dehaze) / 100.0f * 1.4f;
    constexpr float kHazeFloor = 0.12f;
    const float vib = static_cast<float>(a.vibrance) / 100.0f;

#ifdef LRC_ENABLE_SIMD
    lrc::parallel::parallelFor(
        pixelCount, lrc::parallel::defaultGrain(pixelCount),
        [&](size_t iBegin, size_t iEnd) {
            applyStageCBasicSIMD(p, iBegin, iEnd, rTemp, gTint, bTemp,
                                 dhBoost, kHazeFloor, a.dehaze != 0.0, vib);
        });
#else
    lrc::parallel::parallelFor(
        pixelCount, lrc::parallel::defaultGrain(pixelCount),
        [&](size_t iBegin, size_t iEnd) {
    for (size_t i = iBegin; i < iEnd; ++i) {
        float r = p[i * 3 + 0];
        float g = p[i * 3 + 1];
        float b = p[i * 3 + 2];

        // 5. White balance (temperature / tint around the as-shot WB).
        r *= rTemp;
        b *= bTemp;
        g *= gTint;

        // 5.5 Dehaze (atmospheric haze removal / addition).
        if (a.dehaze != 0.0) {
            const float floor = kHazeFloor * (1.0f - dhBoost);
            r = floor + (r - floor) * dhBoost;
            g = floor + (g - floor) * dhBoost;
            b = floor + (b - floor) * dhBoost;
            r = std::max(r, 0.0f);
            g = std::max(g, 0.0f);
            b = std::max(b, 0.0f);
        }

        // 5.75 Vibrance: saturation that acts most on muted colours and
        //      protects skin tones (orange/yellow hues).
        if (vib != 0.0f) {
            float hue, csat, luma;
            rgbToHSL(r, g, b, hue, csat, luma);
            float d = std::abs(hue - 42.0f);
            if (d > 180.0f) d = 360.0f - d;
            const float skinWeight = 1.0f - smoothstep01(d / 45.0f);
            const float boost =
                1.0f + vib * (1.0f - 0.75f * skinWeight) * (1.0f - csat);
            const float scale = csat > 1e-6f ? boost : 1.0f;
            r = luma + (r - luma) * scale;
            g = luma + (g - luma) * scale;
            b = luma + (b - luma) * scale;
        }

        p[i * 3 + 0] = r;
        p[i * 3 + 1] = g;
        p[i * 3 + 2] = b;
    }
    });
#endif
}

// Stage D: HSL per-channel -> global saturation -> split toning -> point colour.
// This is the final per-pixel stage, so it also clamps the result into [0,1]
// (the original applyAdjustments clamps once at the end of its single loop).
void applyStageD(const Adjustments& a, float* p, size_t pixelCount,
                 uint8_t activeMask) {
    // Any bit set means at least one HSL channel is active. activeMask is
    // threaded into applyHSLPerChannel so all-zero channels are skipped inside
    // the per-pixel loop.
    const bool useHsl = (activeMask != 0u);
    const float sat = 1.0f + static_cast<float>(a.saturation) / 100.0f;
    const bool useSat = (a.saturation != 0.0);
    const bool useSplit =
        a.splitTone.highlightSaturation >= 0.5 ||
        a.splitTone.shadowSaturation >= 0.5;
    const std::vector<ColorTargetPrecomputed> ctData =
        precomputeColorTargets(a.colorTargets);
    const bool anyWork = useHsl || useSat || useSplit || !ctData.empty();

    lrc::parallel::parallelFor(
        pixelCount, lrc::parallel::defaultGrain(pixelCount),
        [&](size_t iBegin, size_t iEnd) {
    for (size_t i = iBegin; i < iEnd; ++i) {
        float r = p[i * 3 + 0];
        float g = p[i * 3 + 1];
        float b = p[i * 3 + 2];
        if (anyWork) {
            // 6. HSL: per-channel adjustments (hue, saturation, luminance).
            if (useHsl)
                applyHSLPerChannel(r, g, b, a.hsl, activeMask);

            // Global saturation stacks on top of the per-channel result.
            if (useSat) {
                const float luma =
                    0.2126f * r + 0.7152f * g + 0.0722f * b;
                r = luma + (r - luma) * sat;
                g = luma + (g - luma) * sat;
                b = luma + (b - luma) * sat;
            }

            // 7. Split toning: separate colour tint for highlights and shadows.
            if (useSplit)
                applySplitTone(r, g, b, a.splitTone);

            // 7.5 Point-colour targets: colour-keyed HSL adjustments.
            for (const ColorTargetPrecomputed& ct : ctData)
                applyColorTarget(r, g, b, ct);
        }
        p[i * 3 + 0] = std::max(0.0f, std::min(r, 1.0f));
        p[i * 3 + 1] = std::max(0.0f, std::min(g, 1.0f));
        p[i * 3 + 2] = std::max(0.0f, std::min(b, 1.0f));
    }
    });
}

// Applies the spatial stages (texture, clarity, sharpening, lens distortion,
// noise reduction, grain, vignette, masked local adjustments) to an F32 RGB
// buffer in place. `sigmaScale` scales the absolute-pixel kernels (texture,
// sharpening, NR, grain) by the preview/full-resolution rule; the
// buffer-relative kernels (clarity radius) are unaffected.
void applySpatialAdjustmentsScaled(const Adjustments& a, float* p, int w, int h,
                                   float sigmaScale) {
    if (!p || w <= 0 || h <= 0)
        return;
    const size_t n = static_cast<size_t>(w) * h;
    const Pipeline::SpatialKernels kernels =
        Pipeline::spatialKernels(a, w, h, sigmaScale);

    // --- AI Denoise -------------------------------------------------------------
    // Runs BEFORE the manual NR path: the user's manual luma/color NR
    // sliders continue to operate on the AI-denoised buffer, so they can
    // fine-tune residual noise / texture. aiDenoise.strength (0..100) is
    // the mix factor between the original buffer and NAFNet's output
    // (1.0 == full AI replacement). When LRC_ENABLE_AI is off or the
    // engine/model is unavailable, this branch is a no-op and the manual
    // NR sliders below remain the user's primary denoise — preserving
    // bit-identical behaviour with pre-AI builds.
    //
    // Threading: applySpatialAdjustmentsScaled runs on the PreviewWorker
    // thread; globalAiEngine() returns a pointer whose underlying ONNX
    // sessions are per-thread, so concurrent preview + export renders are
    // safe.
    if (a.aiDenoise.hasEffect()) {
#ifdef LRC_ENABLE_AI
        auto* engine = Pipeline::globalAiEngine();
        if (engine && engine->hasModel("nafnet-denoise")) {
            // Wrap p into a fresh F32 buffer (single memcpy; the inference
            // cost dominates). AiDenoiseProcessor reads this buffer and
            // writes a new tiled output, never aliasing the input. The
            // result is memcpy'd back in place.
            auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
            std::memcpy(buf->data().data(), p, n * 3 * sizeof(float));
            AiDenoiseProcessor proc(engine);
            AiDenoiseOptions opts;
            opts.strength = a.aiDenoise.strength / 100.0f;
            opts.luminanceOnly =
                (a.aiDenoise.mode == AiDenoiseMode::LuminanceOnly);
            opts.tileSize = 256;    // NAFNet's native input resolution
            opts.overlap = 64;      // feather-blend band between tiles
            opts.inputSpace = "prophoto";  // working space, no sRGB round-trip
            auto denoised = proc.denoise(*buf, "nafnet-denoise", opts);
            if (denoised) {
                std::memcpy(p, denoised->data().data(),
                            n * 3 * sizeof(float));
            }
        }
#endif
        // No-branch when AI is disabled or the model is not loaded: the
        // manual luma/color NR kernels below remain authoritative.
    }

    // --- Texture + clarity: local (high-pass) contrast on luminance. --------
    // A single luma pass is shared by both stages (the base image is unchanged
    // inside this block). The blur temporaries come from the scratch pool.
    if (a.texture != 0.0 || a.clarity != 0.0) {
        ScratchBuffer luma(n);
        lumaImage(p, w, h, luma.vec());

        if (a.texture != 0.0) {
            ScratchBuffer blurred(n);
            // P3-4: texture blur routes through the switchable spatial-filter
            // backend (CPU by default; D3D11 DirectCompute when compiled in and
            // the device probe succeeds). Output contract is identical either way.
            pipelineSpatialFilterBackend()->blur(luma.data(), w, h,
                                                 kernels.textureSigma,
                                                 blurred.vec());
            const float gain =
                static_cast<float>(a.texture) / 100.0f * 0.30f;
            lrc::parallel::parallelFor(n, lrc::parallel::defaultGrain(n),
                                       [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    const float d = (luma[i] - blurred[i]) * gain;
                    p[i * 3 + 0] += d;
                    p[i * 3 + 1] += d;
                    p[i * 3 + 2] += d;
                }
            });
        }

        if (a.clarity != 0.0) {
            // Clarity's blur radius derives from min(w,h)/50, which at full
            // resolution is a very large sigma (a 1920px frame -> ~38) whose
            // three box passes would cover the whole frame. To keep it cheap,
            // the blur runs on a ~1/4-linear-size area-average downsample of
            // the luma, with the sigma scaled by the downsample ratio; the
            // blurred field is bilinearly upsampled back to full resolution
            // and the high-frequency detail is computed there. The texture
            // branch above stays at full resolution (small sigma 2.5).
            const int maxLong = std::max(w, h);
            const int targetLong = std::max(
                1, static_cast<int>(std::lround(static_cast<float>(maxLong) * 0.25f)));
            const float dsScale =
                static_cast<float>(targetLong) / static_cast<float>(maxLong);
            const int dw = std::max(
                1, static_cast<int>(std::lround(static_cast<float>(w) * dsScale)));
            const int dh = std::max(
                1, static_cast<int>(std::lround(static_cast<float>(h) * dsScale)));

            ScratchBuffer dsLuma(static_cast<size_t>(dw) * dh);
            downsampleLumaArea(luma.data(), w, h, dw, dh, dsLuma.vec());
            ScratchBuffer dsBlurred(static_cast<size_t>(dw) * dh);
            gaussianBlur(dsLuma.data(), dw, dh, kernels.clarityRadius * dsScale,
                         dsBlurred.vec());
            ScratchBuffer blurred(n);
            upsampleLumaBilinear(dsBlurred.data(), dw, dh, w, h, blurred.vec());

            const float gain =
                static_cast<float>(a.clarity) / 100.0f * 0.45f;
            lrc::parallel::parallelFor(n, lrc::parallel::defaultGrain(n),
                                       [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    const float L = luma[i];
                    // Protect flat near-black / near-white regions from halos.
                    const float protect =
                        smoothstep01(L / 0.05f) *
                        (1.0f - smoothstep01((L - 0.92f) / 0.08f));
                    const float d = (L - blurred[i]) * gain * protect;
                    p[i * 3 + 0] += d;
                    p[i * 3 + 1] += d;
                    p[i * 3 + 2] += d;
                }
            });
        }
    }

    // --- Noise reduction: YCbCr (BT.709) split, edge-preserving luma NR and
    // chroma NR. Runs before sharpening so noise is not amplified. ------
    // Neutral fast path: both sliders at the dead-zone (<= 0.5) skip the whole
    // stage, so a default parameter stack costs nothing here.
    if (a.luminance > 0.5 || a.colorNoiseReduction > 0.5) {
        ScratchBuffer yBuf(n), cbBuf(n), crBuf(n);
        float* Y = yBuf.data();
        float* Cb = cbBuf.data();
        float* Cr = crBuf.data();

        // Convert the working RGB to BT.709 YCbCr (exact forward transform).
        lrc::parallel::parallelFor(n, lrc::parallel::defaultGrain(n),
                                   [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const float r = p[i * 3 + 0];
                const float g = p[i * 3 + 1];
                const float b = p[i * 3 + 2];
                Y[i] = rec709Luma(r, g, b);
                Cb[i] = (b - Y[i]) / 1.8556f;
                Cr[i] = (r - Y[i]) / 1.5748f;
            }
        });

        // Chroma NR: blur Cb/Cr. Sigma grows with both the colour and the
        // smoothness sliders; a strength-weighted blend toward the blurred
        // value avoids over-softening fine colour detail.
        if (a.colorNoiseReduction > 0.5) {
            const float cMix = std::clamp(
                static_cast<float>(a.colorNoiseReduction) / 100.0f, 0.0f, 1.0f);
            ScratchBuffer blurred(n);
            gaussianBlur(Cb, w, h, kernels.colorNRSigma, blurred.vec());
            lrc::parallel::parallelFor(n, lrc::parallel::defaultGrain(n),
                                       [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i)
                    Cb[i] += (blurred[i] - Cb[i]) * cMix;
            });
            gaussianBlur(Cr, w, h, kernels.colorNRSigma, blurred.vec());
            lrc::parallel::parallelFor(n, lrc::parallel::defaultGrain(n),
                                       [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i)
                    Cr[i] += (blurred[i] - Cr[i]) * cMix;
            });
        }

        // Luma NR: split luma into a small-sigma base and the residual
        // high-frequency; attenuate the residual where a 3x3 local-variance
        // estimate says the area is flat and keep it where an edge exists.
        // luminanceDetail raises the edge threshold (more detail preserved).
        if (a.luminance > 0.5) {
            ScratchBuffer yBlur(n);
            gaussianBlur(Y, w, h, kernels.lumaNRSigma, yBlur.vec());

            const float detailF =
                static_cast<float>(a.luminanceDetail) / 100.0f;
            const float edgeThresh = 0.0002f + detailF * 0.0008f;
            const float nrStrength = static_cast<float>(a.luminance) / 100.0f;
            ScratchBuffer edge(n);

            // Per-pixel local variance of Y over a clamped 3x3 window.
            const size_t rowGrain =
                lrc::parallel::defaultGrain(static_cast<size_t>(h));
            lrc::parallel::parallelFor(static_cast<size_t>(h), rowGrain,
                                       [&](size_t by, size_t ey) {
                for (int y = static_cast<int>(by); y < static_cast<int>(ey); ++y) {
                    for (int x = 0; x < w; ++x) {
                        float sum = 0.0f, sum2 = 0.0f;
                        for (int dy = -1; dy <= 1; ++dy) {
                            const int sy = std::clamp(y + dy, 0, h - 1);
                            const float* row = Y + static_cast<size_t>(sy) * w;
                            for (int dx = -1; dx <= 1; ++dx) {
                                const float v = row[std::clamp(x + dx, 0, w - 1)];
                                sum += v;
                                sum2 += v * v;
                            }
                        }
                        const float mean = sum * (1.0f / 9.0f);
                        const float var = sum2 * (1.0f / 9.0f) - mean * mean;
                        edge[static_cast<size_t>(y) * w + x] =
                            std::max(var, 0.0f);
                    }
                }
            });

            // Adaptive attenuation: flat areas lose the residual, edges keep it.
            lrc::parallel::parallelFor(n, lrc::parallel::defaultGrain(n),
                                       [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    const float hf = Y[i] - yBlur[i];
                    const float flatness =
                        1.0f - smoothstep01(edge[i] / edgeThresh);
                    Y[i] -= hf * nrStrength * flatness;
                }
            });

            // Local-contrast restore: a wide unsharp on the denoised luma
            // brings back mid-frequency contrast that the NR flattened.
            if (a.luminanceContrast > 0.5) {
                const float lcGain =
                    static_cast<float>(a.luminanceContrast) / 100.0f * 0.8f;
                gaussianBlur(Y, w, h, kernels.lumaContrastSigma, yBlur.vec());
                lrc::parallel::parallelFor(n, lrc::parallel::defaultGrain(n),
                                           [&](size_t begin, size_t end) {
                    for (size_t i = begin; i < end; ++i)
                        Y[i] += (Y[i] - yBlur[i]) * lcGain;
                });
            }
        }

        // Reconstruct linear RGB from the adjusted YCbCr (exact inverse).
        lrc::parallel::parallelFor(n, lrc::parallel::defaultGrain(n),
                                   [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const float yy = Y[i];
                const float rr = yy + 1.5748f * Cr[i];
                const float bb = yy + 1.8556f * Cb[i];
                const float gg =
                    (yy - 0.2126f * rr - 0.0722f * bb) / 0.7152f;
                p[i * 3 + 0] = rr;
                p[i * 3 + 1] = gg;
                p[i * 3 + 2] = bb;
            }
        });
    }

    // --- Sharpening: luma unsharp mask with detail + edge masking. ----------
    if (a.sharpness > 0.5) {
        const float amount = static_cast<float>(a.sharpness) / 100.0f * 1.2f;
        // Sharpening runs after texture/clarity have modified p in place, so
        // its luma must be recomputed from the updated buffer (sharing the
        // luma above would change the output). Both buffers are pooled.
        ScratchBuffer luma(n);
        lumaImage(p, w, h, luma.vec());
        ScratchBuffer blurred(n);
        // P3-4: sharpening's luma blur routes through the switchable
        // spatial-filter backend (see the texture branch above).
        pipelineSpatialFilterBackend()->blur(luma.data(), w, h,
                                             kernels.sharpnessSigma,
                                             blurred.vec());

        // Sobel edge magnitude map used by the "masking" slider. Each output
        // row only writes its own pixels (neighbour reads are shared), so the
        // loop splits cleanly over rows.
        std::vector<float> edges(n, 0.0f);
        // Any masking value must mask: compute the edge map whenever the
        // slider is above zero so edgeScale below reads real edges. (masking
        // == 0 skips the map and leaves edgeScale at 1.0 -- full sharpening.)
        if (a.sharpnessMasking > 0.0) {
            const size_t rowGrain =
                lrc::parallel::defaultGrain(static_cast<size_t>(h));
            lrc::parallel::parallelFor(static_cast<size_t>(h), rowGrain,
                                       [&](size_t by, size_t ey) {
                const int y0 = std::max(1, static_cast<int>(by));
                const int y1 = std::min(h - 1, static_cast<int>(ey));
                for (int y = y0; y < y1; ++y) {
                    for (int x = 1; x < w - 1; ++x) {
                        const size_t i = static_cast<size_t>(y) * w + x;
                        const float gx = -luma[i - w - 1] + luma[i - w + 1] -
                                         2.0f * luma[i - 1] + 2.0f * luma[i + 1] -
                                         luma[i + w - 1] + luma[i + w + 1];
                        const float gy = -luma[i - w - 1] - 2.0f * luma[i - w] -
                                         luma[i - w + 1] + luma[i + w - 1] +
                                         2.0f * luma[i + w] + luma[i + w + 1];
                        edges[i] = std::sqrt(gx * gx + gy * gy);
                    }
                }
            });
        }
        const float maskThresh =
            static_cast<float>(a.sharpnessMasking) / 100.0f;
        const float detailF = static_cast<float>(a.sharpnessDetail) / 100.0f;

        lrc::parallel::parallelFor(n, lrc::parallel::defaultGrain(n),
                                   [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const float hp = luma[i] - blurred[i];
                const float amp = std::abs(hp);
                float detailScale = 1.0f;
                if (detailF > 1e-3f) {
                    // High detail keeps stronger edges, damps fine ones.
                    detailScale = 0.3f +
                        0.7f * smoothstep01(amp / (0.02f + detailF * 0.12f));
                }
                float edgeScale = 1.0f;
                if (maskThresh > 1e-3f) {
                    edgeScale = smoothstep01(
                        (edges[i] - maskThresh * 0.6f) /
                        (0.15f + maskThresh * 0.5f));
                }
                const float d = hp * amount * detailScale * edgeScale;
                p[i * 3 + 0] += d;
                p[i * 3 + 1] += d;
                p[i * 3 + 2] += d;
            }
        });
    }

    // --- Lensfun geometric distortion correction -----------------------------
    // Runs before the manual radial distortion slider so the two can stack:
    // Lensfun undoes the lens's optical distortion from the lens profile, and
    // the manual `distortion` slider lets the user fine-tune on top. When
    // Lensfun is disabled or the profile/capture parameters are unavailable,
    // applyGeometry returns false and leaves the buffer untouched.
    if (a.lensfunEnabled)
        LensfunProfile::applyGeometry(a, p, w, h);

    // --- Lens distortion: radial remap (barrel / pincushion). ---------------
    if (a.distortion != 0.0) {
        const float k = static_cast<float>(a.distortion) / 100.0f * 0.10f;
        const float cx = (static_cast<float>(w) - 1.0f) * 0.5f;
        const float cy = (static_cast<float>(h) - 1.0f) * 0.5f;
        std::vector<float> out(p, p + n * 3);
        const size_t rowGrain =
            lrc::parallel::defaultGrain(static_cast<size_t>(h));
        lrc::parallel::parallelFor(static_cast<size_t>(h), rowGrain,
                                   [&](size_t by, size_t ey) {
            for (int y = static_cast<int>(by); y < static_cast<int>(ey); ++y) {
                for (int x = 0; x < w; ++x) {
                    const float nx = (static_cast<float>(x) - cx) /
                                     (cx > 1.0f ? cx : 1.0f);
                    const float ny = (static_cast<float>(y) - cy) /
                                     (cy > 1.0f ? cy : 1.0f);
                    const float r2 = nx * nx + ny * ny;
                    const float scale = 1.0f + k * r2;
                    const float sx = cx + nx * cx * scale;
                    const float sy = cy + ny * cy * scale;
                    float r, g, b;
                    sampleRGB(out.data(), w, h, sx, sy, r, g, b);
                    p[(static_cast<size_t>(y) * w + x) * 3 + 0] = r;
                    p[(static_cast<size_t>(y) * w + x) * 3 + 1] = g;
                    p[(static_cast<size_t>(y) * w + x) * 3 + 2] = b;
                }
            }
        });
    }

    // --- Film grain: additive value noise scaled by luma. --------------------
    if (a.grain > 0.5) {
        const float amount = static_cast<float>(a.grain) / 100.0f * 0.05f;
        // size: 100 -> coarse (cell ~48 px), 0 -> fine (cell ~4 px).
        const float cell = kernels.grainCell;
        // roughness mixes a high-frequency lattice into the smooth one.
        const float rough = static_cast<float>(a.grainRoughness) / 100.0f;
        const float highCell = std::max(1.5f, cell * 0.25f);
        const size_t rowGrain =
            lrc::parallel::defaultGrain(static_cast<size_t>(h));
        lrc::parallel::parallelFor(static_cast<size_t>(h), rowGrain,
                                   [&](size_t by, size_t ey) {
            for (int y = static_cast<int>(by); y < static_cast<int>(ey); ++y) {
                for (int x = 0; x < w; ++x) {
                    size_t i = (static_cast<size_t>(y) * w + x) * 3;
                    const float luma = rec709Luma(p[i], p[i + 1], p[i + 2]);
                    float n = grainValueNoise(static_cast<float>(x),
                                              static_cast<float>(y), cell);
                    if (rough > 0.02f) {
                        const float hi = grainValueNoise(
                            static_cast<float>(x), static_cast<float>(y),
                            highCell);
                        n = n * (1.0f - rough) + hi * rough;
                    }
                    const float delta =
                        (n - 0.5f) * amount * (0.25f + 0.75f * luma);
                    p[i] += delta;
                    p[i + 1] += delta;
                    p[i + 2] += delta;
                }
            }
        });
    }

    // --- Post-crop vignette: radial corner shading centred on the buffer's
    // geometric centre (the post-crop image centre). ---------------------
    if (a.vignette != 0.0) {
        const float amount = static_cast<float>(a.vignette) / 100.0f;
        const auto weightField = Pipeline::postCropVignetteWeightField(a, w, h);
        // Highlight Priority (style 2) protects bright pixels from the
        // darkening (an optical-vignette behaviour). Paint Overlay / Colour
        // Priority are TODO; they share the geometric weight but
        // skip the highlight interaction.
        const bool highlightPriority = (a.vignetteStyle == 2);
        const float hiNorm = std::clamp(
            static_cast<float>(a.vignetteHighlights) / 100.0f, 0.0f, 1.0f);
        const bool useHighlights = highlightPriority && hiNorm > 0.0f;
        const size_t rowGrain =
            lrc::parallel::defaultGrain(static_cast<size_t>(h));
        lrc::parallel::parallelFor(static_cast<size_t>(h), rowGrain,
                                   [&](size_t by, size_t ey) {
            for (int y = static_cast<int>(by); y < static_cast<int>(ey); ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    float wgt = (*weightField)[i];
                    if (useHighlights) {
                        // Luma mask: 1 for bright pixels, 0 for dark ones.
                        const float luma =
                            rec709Luma(p[i * 3], p[i * 3 + 1], p[i * 3 + 2]);
                        const float hw = smoothstep01((luma - 0.5f) / 0.5f);
                        wgt *= 1.0f - hiNorm * hw;
                    }
                    const float factor = 1.0f + amount * wgt;
                    p[i * 3] *= factor;
                    p[i * 3 + 1] *= factor;
                    p[i * 3 + 2] *= factor;
                }
            }
        });
    }

    // --- Masked local adjustments: each mask's own adjustment stack, blended
    // by mask weight * opacity on top of the globally-adjusted image. --------
    if (!a.masks.empty())
        Pipeline::applyLocalAdjustments(a, p, w, h);

    // --- Spot removal / healing brush -------------------------------------------
    // Runs after every spatial stage, on the linear ProPhoto F32 working buffer
    // and before the final sRGB conversion. A no-op when the adjustment carries
    // no spots, so the neutral parameter stack is untouched (bit-exact).
    Pipeline::applySpotRemoval(a, p, w, h);

    // Final clamp into [0,1].
    lrc::parallel::parallelFor(n * 3, lrc::parallel::defaultGrain(n * 3),
                               [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i)
            p[i] = std::clamp(p[i], 0.0f, 1.0f);
    });
}

// --- Display-encoded histogram helpers ----------------------------------------

// LUT resolution for the non-linear segment of the sRGB OETF. 4096 entries keep
// the worst-case bin quantization far below one bin width (the OETF's max slope
// over the covered range is < 1, so an input cell of 1/4096 maps to well under
// 1/256 of output range); the linear near-black segment is evaluated exactly
// instead, where the 12.92x slope would amplify LUT error.
constexpr size_t kSrgbBinLutSize = 4096;

// LUT entry i holds bin = floor(srgbEncode((i + 0.5) / kSrgbBinLutSize) * 256)
// for the sRGB OETF (IEC 61966-2-1). Built once via thread-safe static
// initialization; read-only afterwards. Indexed only for the gamma segment
// (see linearToHistogramBin).
const std::array<uint8_t, kSrgbBinLutSize>& srgbBinLut() {
    static const std::array<uint8_t, kSrgbBinLutSize> lut = [] {
        std::array<uint8_t, kSrgbBinLutSize> t{};
        for (size_t i = 0; i < kSrgbBinLutSize; ++i) {
            const float v = (static_cast<float>(i) + 0.5f) / kSrgbBinLutSize;
            const float enc = (v <= 0.0031308f)
                                  ? 12.92f * v
                                  : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
            const int bin = static_cast<int>(std::floor(enc * 256.0f));
            t[i] = static_cast<uint8_t>(std::clamp(bin, 0, 255));
        }
        return t;
    }();
    return lut;
}

// Maps one linear-light channel value in [0,1] to a display-encoded bin in
// [0,255]. The sRGB OETF turns linear light into the display domain so the
// histogram reflects what the user sees; bin = floor(encoded * 256) keeps the
// bin boundaries at the same code values as the U8 fallback path.
inline uint8_t linearToHistogramBin(float v) {
    v = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
    if (v <= 0.0031308f) {
        const int bin = static_cast<int>(std::floor(12.92f * v * 256.0f));
        return static_cast<uint8_t>(std::clamp(bin, 0, 255));
    }
    const int idx = static_cast<int>(v * kSrgbBinLutSize);
    return srgbBinLut()[static_cast<size_t>(
        std::clamp(idx, 0, static_cast<int>(kSrgbBinLutSize - 1)))];
}

} // namespace

namespace detail {

// P3-4 bridge: exposes the file-local gaussianBlur (anonymous namespace above)
// to the spatial-filter backends through the detail::gaussianBlur contract
// (declared in engine/GaussianBlur.h). A single call hop; the implementation
// itself is untouched, so CPU-backend output is byte-identical to the legacy
// path. `::lrc::gaussianBlur` is valid within this TU: unnamed-namespace
// members are referable as members of the enclosing namespace here.
void gaussianBlur(const float* src, int w, int h, float sigma,
                  std::vector<float>& out) {
    ::lrc::gaussianBlur(src, w, h, sigma, out);
}

} // namespace detail

// Test hook: exposes the file-local gaussianBlur (exact for sigma <= 4, box
// path for sigma > 4) so unit tests can verify the fast path numerically.
// Declared locally in tests/TestPipeline.cpp; not part of the public API.
std::vector<float> pipelineGaussianBlurTest(const float* src, int w, int h,
                                            float sigma) {
    std::vector<float> out;
    gaussianBlur(src, w, h, sigma, out);
    return out;
}

HistogramBins Pipeline::computeHistogramFromLinear(
    const ImageBuffer& buffer) {
    HistogramBins bins{};
    if (buffer.empty() || buffer.channels() < 3)
        return bins;
    const size_t pixelCount =
        static_cast<size_t>(buffer.width()) * buffer.height();

    if (buffer.format() == PixelFormat::F32) {
        const float* p =
            reinterpret_cast<const float*>(buffer.data().data());
        for (size_t i = 0; i < pixelCount; ++i, p += 3) {
            ++bins[0][linearToHistogramBin(p[0])];
            ++bins[1][linearToHistogramBin(p[1])];
            ++bins[2][linearToHistogramBin(p[2])];
        }
    } else if (buffer.format() == PixelFormat::U16) {
        const uint16_t* p =
            reinterpret_cast<const uint16_t*>(buffer.data().data());
        for (size_t i = 0; i < pixelCount; ++i, p += 3) {
            ++bins[0][linearToHistogramBin(p[0] / 65535.0f)];
            ++bins[1][linearToHistogramBin(p[1] / 65535.0f)];
            ++bins[2][linearToHistogramBin(p[2] / 65535.0f)];
        }
    } else if (buffer.format() == PixelFormat::U8) {
        // Display-ready sRGB: the stored byte already is the display-encoded
        // value, so bin = byte keeps this fallback path bit-identical to the
        // legacy 8-bit preview histogram.
        const uint8_t* p = buffer.data().data();
        for (size_t i = 0; i < pixelCount; ++i, p += 3) {
            ++bins[0][p[0]];
            ++bins[1][p[1]];
            ++bins[2][p[2]];
        }
    }
    return bins;
}

float Pipeline::maskWeight(const Mask& m, int w, int h, float nx, float ny) {
    switch (m.type) {
        case MaskType::LinearGradient: {
            const float dx = m.linear.endX - m.linear.startX;
            const float dy = m.linear.endY - m.linear.startY;
            const float len2 = dx * dx + dy * dy;
            if (len2 < 1e-6f)
                return m.invert ? 1.0f : 0.0f;
            // s is the projection of the pixel onto the gradient axis, 0 at the
            // start line and 1 at the end line. The mask covers the strip s in
            // [0,1] and fades out within `feather` on both sides.
            const float s = ((nx - m.linear.startX) * dx +
                             (ny - m.linear.startY) * dy) / len2;
            const float soft = m.linear.feather * 0.35f;
            float wgt = 1.0f;
            if (s < soft)
                wgt = std::min(wgt, smoothstep01(
                    std::max(s, 0.0f) / std::max(soft, 1e-4f)));
            if (s > 1.0f - soft)
                wgt = std::min(wgt, 1.0f - smoothstep01(
                    (s - (1.0f - soft)) / std::max(soft, 1e-4f)));
            if (m.invert)
                wgt = 1.0f - wgt;
            return std::clamp(wgt, 0.0f, 1.0f);
        }
        case MaskType::RadialGradient: {
            const float rx = std::max(m.radial.radiusX, 1e-4f);
            const float ry = std::max(m.radial.radiusY, 1e-4f);
            const float dx = (nx - m.radial.centerX) / rx;
            const float dy = (ny - m.radial.centerY) / ry;
            const float d = std::sqrt(dx * dx + dy * dy);
            // Falloff begins at radius fraction (1 - feather); feather 0 is a
            // hard ellipse, feather 1 fades across the whole radius.
            const float inner = 1.0f - m.radial.feather;
            const float t = (d - inner) / std::max(m.radial.feather, 1e-4f);
            float falloff = 1.0f - smoothstep01(std::clamp(t, 0.0f, 1.0f));
            if (m.radial.invert)
                falloff = 1.0f - falloff;
            if (m.invert)
                falloff = 1.0f - falloff;
            return std::clamp(falloff, 0.0f, 1.0f);
        }
        case MaskType::Brush: {
            if (m.brush.empty())
                return m.invert ? 1.0f : 0.0f;
            const float minDim = static_cast<float>(std::min(w, h));
            float wgt = 0.0f;
            for (const BrushDab& dab : m.brush) {
                const float rx = std::max(
                    dab.radius * minDim / std::max(w, 1), 1e-4f);
                const float ry = std::max(
                    dab.radius * minDim / std::max(h, 1), 1e-4f);
                const float dx = (nx - dab.x) / rx;
                const float dy = (ny - dab.y) / ry;
                const float d = std::sqrt(dx * dx + dy * dy);
                const float soft = std::max(dab.softness, 1e-4f);
                const float wi = 1.0f - smoothstep01(
                    (d - (1.0f - soft)) / soft);
                wgt = std::max(wgt, wi);
            }
            if (m.invert)
                wgt = 1.0f - wgt;
            return std::clamp(wgt, 0.0f, 1.0f);
        }
        case MaskType::AiMask:
            // AiMask is content-driven (semantic segmentation) and can only
            // be evaluated with pixel access in applyLocalAdjustments. The
            // pure-geometry maskWeight path has no buffer to run the model
            // on, so return a neutral 0.5 weight — visible in the Alt-overlay
            // but non-authoritative. The real weight is generated inline in
            // applyLocalAdjustments via AiMaskGenerator::generate.
            return 0.5f;
        case MaskType::None:
        default:
            return m.invert ? 1.0f : 0.0f;
    }
}

std::shared_ptr<const std::vector<float>>
Pipeline::brushWeightField(const Mask& mask, int w, int h) {
    if (w <= 0 || h <= 0)
        return {};
    const size_t keyHash = mask.geometryHash();
    const size_t bytes =
        sizeof(float) * static_cast<size_t>(w) * static_cast<size_t>(h);

    // Hit path: scan under the lock and return the shared entry. The caller
    // keeps the shared_ptr alive after the lock is released, so a concurrent
    // insert/evict never invalidates data a reader already holds (COW-safe).
    {
        std::lock_guard<std::mutex> lock(g_weightMutex);
        for (const WeightEntry& e : g_weightCache) {
            if (e.hash == keyHash && e.w == w && e.h == h)
                return e.field;
        }
    }

    // Miss: rasterize outside the lock so the mutex is never held during a
    // potentially large buffer build. The shared_ptr<const> conversion keeps
    // the buffer immutable for every reader.
    auto field = std::make_shared<std::vector<float>>(
        rasterizeMaskField(mask, w, h));

    std::lock_guard<std::mutex> lock(g_weightMutex);
    // Another thread may have inserted the same key while we rasterized; reuse
    // its buffer (the content is deterministic, so both are identical).
    for (const WeightEntry& e : g_weightCache) {
        if (e.hash == keyHash && e.w == w && e.h == h)
            return e.field;
    }
    if (g_weightCache.capacity() < 64)
        g_weightCache.reserve(64);
    g_weightCache.push_back(WeightEntry{keyHash, w, h, field});
    g_weightCacheBytes += bytes;
    evictWeightCacheLocked();
    return field;
}

std::shared_ptr<const std::vector<float>>
Pipeline::postCropVignetteWeightField(const Adjustments& a, int w, int h) {
    if (w <= 0 || h <= 0)
        return {};
    // Key: midpoint / roundness / feather / style + buffer size. Amount and
    // highlights are deliberately absent so a pure amount change reuses the
    // cached field without a full recompute.
    size_t hh = 1469598103934665603ull;
    mixVignetteKey(hh, a.vignetteMidpoint);
    mixVignetteKey(hh, a.vignetteRoundness);
    mixVignetteKey(hh, a.vignetteFeather);
    hh = hashCombine(hh, static_cast<size_t>(a.vignetteStyle));
    hh = hashCombine(hh, static_cast<size_t>(static_cast<uint32_t>(w)));
    hh = hashCombine(hh, static_cast<size_t>(static_cast<uint32_t>(h)));
    const size_t keyHash = hh;
    const size_t bytes =
        sizeof(float) * static_cast<size_t>(w) * static_cast<size_t>(h);

    // Hit path: scan under the lock and return the shared entry (the caller
    // keeps the shared_ptr alive after the lock is released, so a concurrent
    // insert/evict never invalidates data a reader already holds).
    {
        std::lock_guard<std::mutex> lock(g_vignetteMutex);
        for (const VignetteEntry& e : g_vignetteCache) {
            if (e.hash == keyHash && e.w == w && e.h == h)
                return e.field;
        }
    }

    // Miss: rasterize outside the lock so the mutex is never held during a
    // potentially large buffer build.
    auto field = std::make_shared<std::vector<float>>(
        computeVignetteWeightField(a, w, h));

    std::lock_guard<std::mutex> lock(g_vignetteMutex);
    // Another thread may have inserted the same key while we rasterized; reuse
    // its buffer (the content is deterministic, so both are identical).
    for (const VignetteEntry& e : g_vignetteCache) {
        if (e.hash == keyHash && e.w == w && e.h == h)
            return e.field;
    }
    if (g_vignetteCache.capacity() < 32)
        g_vignetteCache.reserve(32);
    g_vignetteCache.push_back(VignetteEntry{keyHash, w, h, field});
    g_vignetteCacheBytes += bytes;
    evictVignetteCacheLocked();
    return field;
}

// Per-pixel weight [0,1] for a colour-range mask, compared in HSL space. r/g/b
// are linear RGB [0,1]. A mask without a sampled reference is inert (0).
float Pipeline::colorRangeWeight(const ColorRangeMask& m, float r, float g,
                                 float b) {
    if (!m.hasReference) return 0.0f;
    float ph, ps, pl, rh, rs, rl;
    rgbToHSL(r, g, b, ph, ps, pl);
    rgbToHSL(m.refR, m.refG, m.refB, rh, rs, rl);
    // hue is in degrees [0,360); reduce to the shortest angular distance, then
    // normalize so dhN stays within [0,1].
    float dh = std::fabs(ph - rh);
    if (dh > 180.0f) dh = 360.0f - dh;
    const float dhN = dh / 180.0f;
    const float dsN = std::fabs(ps - rs);
    const float dlN = std::fabs(pl - rl);
    const float hw = rangeAxisWeight(dhN, m.hueTolerance / 180.0, m.softness);
    const float sw = rangeAxisWeight(dsN, m.satTolerance, m.softness);
    const float lw = rangeAxisWeight(dlN, m.lumaTolerance, m.softness);
    const float satFloor = smoothstep01(ps / 0.06f);  // grey gate
    return hw * sw * lw * satFloor;
}

// --- Sub-mask weight helpers ------------------------------------------------

float Pipeline::colorSubMaskWeight(const Mask& m, float r, float g, float b) {
    if (!m.useColorRange)
        return 1.0f;  // identity element of the product (intersection).
    float ph, ps, pl, rh, rs, rl;
    rgbToHSL(r, g, b, ph, ps, pl);
    rgbToHSL(m.colorRefR, m.colorRefG, m.colorRefB, rh, rs, rl);
    // Hue distance reduced to the shortest angular distance, normalized so
    // dhN stays within [0,1] (1 == 180 degrees away).
    float dh = std::fabs(ph - rh);
    if (dh > 180.0f) dh = 360.0f - dh;
    const float dhN = dh / 180.0f;
    const float dsN = std::fabs(ps - rs);
    const float dlN = std::fabs(pl - rl);
    // Combined distance: the max of the three normalized axes, so a pixel must
    // be close to the reference in hue, saturation AND luma to be selected.
    const float d = std::max({dhN, dsN, dlN});
    // `colorRange` (0-100) maps to the band width; `colorSmoothness` (0-100)
    // maps to the smoothstep transition softness at the band edge.
    return rangeAxisWeight(d, m.colorRange / 100.0f,
                           m.colorSmoothness / 100.0f);
}

float Pipeline::luminanceSubMaskWeight(const Mask& m, float r, float g,
                                       float b) {
    if (!m.useLuminance)
        return 1.0f;  // identity element of the product (intersection).
    const float L = 0.2126f * r + 0.7152f * g + 0.0722f * b;  // [0,1]
    const float refL = m.luminanceRef / 100.0f;               // [0,1]
    const float dl = std::fabs(L - refL);
    return rangeAxisWeight(dl, m.luminanceRange / 100.0f,
                           m.luminanceSmoothness / 100.0f);
}

float Pipeline::combinedMaskWeight(const Mask& m, int w, int h, float nx,
                                   float ny, float r, float g, float b) {
    const float geo = Pipeline::maskWeight(m, w, h, nx, ny);  // invert folded.
    if (!m.useColorRange && !m.useLuminance)
        return std::clamp(geo, 0.0f, 1.0f);  // no sub-mask: geometry weight only.
    if (m.maskUnion) {
        float best = geo;
        if (m.useColorRange)
            best = std::max(best, colorSubMaskWeight(m, r, g, b));
        if (m.useLuminance)
            best = std::max(best, luminanceSubMaskWeight(m, r, g, b));
        return std::clamp(best, 0.0f, 1.0f);
    }
    float wgt = geo;
    if (m.useColorRange)
        wgt *= colorSubMaskWeight(m, r, g, b);
    if (m.useLuminance)
        wgt *= luminanceSubMaskWeight(m, r, g, b);
    return std::clamp(wgt, 0.0f, 1.0f);
}

void Pipeline::applyLocalAdjustments(const Adjustments& a, float* p, int w,
                                     int h) {
    if (!p || w <= 0 || h <= 0)
        return;
    const size_t n = static_cast<size_t>(w) * h;

    bool anyMask = false;
    for (const Mask& m : a.masks) {
        // A colour-range mask without a sampled reference is inert (weight 0
        // everywhere), so it does not count as an active mask.
        if (m.type != MaskType::None && m.opacity > 0.0 && m.hasEffect() &&
            !(m.type == MaskType::ColorRange && !m.color.hasReference)) {
            anyMask = true;
            break;
        }
    }
    if (!anyMask)
        return;

    // Local clarity needs a blurred luminance field of the base image. Both
    // buffers come from the thread-local scratch pool (no per-call allocation).
    ScratchBuffer luma, blurred;
    bool needClarity = false;
    for (const Mask& m : a.masks)
        if (m.clarity != 0.0) {
            needClarity = true;
            break;
        }
    if (needClarity) {
        luma = ScratchBuffer(n);
        lumaImage(p, w, h, luma.vec());
        const float radius =
            std::max(4.0f, static_cast<float>(std::min(w, h)) / 50.0f);
        blurred = ScratchBuffer(n);
        gaussianBlur(luma.data(), w, h, radius, blurred.vec());
    }

    // Colour-range / sub-mask weights must be evaluated against the base image
    // (the globally adjusted buffer) *before* any mask rewrites pixels in
    // place, otherwise mask order would leak into the weight. Snapshot it once
    // if any active colour-range mask or sub-mask-enabled geometry mask needs
    // it.
    std::vector<float> base;
    bool needBaseSnapshot = false;
    for (const Mask& m : a.masks) {
        if (m.opacity <= 0.0 || !m.hasEffect())
            continue;
        if (m.type == MaskType::ColorRange) {
            if (m.color.hasReference) {
                needBaseSnapshot = true;
                break;
            }
        } else if (m.useColorRange || m.useLuminance) {
            needBaseSnapshot = true;
            break;
        }
    }
    if (needBaseSnapshot) {
        base.resize(n * 3);
        std::memcpy(base.data(), p, n * 3 * sizeof(float));
    }
    const size_t rowGrain =
        lrc::parallel::defaultGrain(static_cast<size_t>(h));

    for (const Mask& m : a.masks) {
        if (m.type == MaskType::None || m.opacity <= 0.0 ||
            (m.type == MaskType::ColorRange && !m.color.hasReference) ||
            !m.hasEffect())
            continue;
        const float opacity = static_cast<float>(std::clamp(m.opacity, 0.0, 1.0));

        // Weight source for this mask.
        //   • Geometry masks (linear/radial/brush): globally cached rasterized
        //     field via brushWeightField (bit-identical to maskWeight per pixel).
        //   • Colour-range masks: HSL distance evaluated per pixel in the blend
        //     loop below (no field needed).
        //   • AiMask : semantic segmentation requires pixel
        //     content, so the field is generated here from the current buffer.
        //     NOT cached globally — the result depends on image pixels and
        //     caching would thrash on every preview frame, evicting geometry
        //     masks. When LRC_ENABLE_AI is off or the engine/model is
        //     unavailable, the mask is skipped (manual geometry masks remain
        //     the user's primary tool).
        std::shared_ptr<const std::vector<float>> field;
        if (m.type == MaskType::ColorRange) {
            // Handled in the blend loop below (per-pixel HSL distance).
        } else if (m.type == MaskType::AiMask) {
#ifdef LRC_ENABLE_AI
            auto* engine = Pipeline::globalAiEngine();
            const std::string modelId = m.ai.modelIdOverride.empty()
                ? std::string(defaultModelIdFor(m.ai.target))
                : m.ai.modelIdOverride;
            // Prompt masks use an encoder+decoder pair rather than a single
            // model, so defaultModelIdFor(Prompt)=="" would fail hasModel().
            // Route them through generate() unconditionally — AiMaskGenerator
            // falls back to a zero mask when the models are unavailable.
            const bool promptMask = (m.ai.target == AiMaskTarget::Prompt);
            if (engine && (promptMask || engine->hasModel(modelId))) {
                // Wrap p into a fresh F32 buffer (single memcpy; the
                // segmentation inference cost dominates). AiMaskGenerator
                // returns an RGB buffer where R=G=B=weight; collapse to a
                // single-channel weight field for the blend loop.
                auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
                std::memcpy(buf->data().data(), p, n * 3 * sizeof(float));
                AiMaskGenerator gen(engine);
                auto maskBuf = gen.generate(*buf, m.ai);
                if (maskBuf) {
                    auto f = std::make_shared<std::vector<float>>(n);
                    const float* mp = reinterpret_cast<const float*>(
                        maskBuf->data().data());
                    for (size_t i = 0; i < n; ++i)
                        (*f)[i] = mp[i * 3];
                    field = f;
                }
            }
#endif
            if (!field) continue;  // AI off / model missing / generation failed
        } else {
            field = Pipeline::brushWeightField(m, w, h);
            if (!field) continue;  // defensive only: never null for valid w/h
        }

        // A geometry mask with colour/luminance sub-mask modifiers
        // builds a combined sub-mask weight field against the base snapshot
        // (the image content decides the pixel selection), then combines it
        // with the cached geometry field per pixel below.
        std::vector<float> subField;
        if (m.type != MaskType::ColorRange && !base.empty() &&
            (m.useColorRange || m.useLuminance)) {
            subField = computeSubMaskField(m, base.data(), w, h);
        }
        const bool hasSubField = !subField.empty();

        // Precompute the local adjustment gains (mirrors the global stack).
        const float exposure = static_cast<float>(std::pow(2.0, m.exposure));
        const float cscale = 1.0f + static_cast<float>(m.contrast) / 100.0f * 0.7f;
        const float highlightsGain =
            1.0f + static_cast<float>(m.highlights) / 100.0f * 0.5f;
        const float shadowsGain =
            1.0f + static_cast<float>(m.shadows) / 100.0f * 0.5f;
        const float wGain = static_cast<float>(m.whites) / 100.0f * 0.9f;
        const float bGain = static_cast<float>(m.blacks) / 100.0f * 0.9f;
        const float rTemp = static_cast<float>(std::pow(2.0, m.temperature * 0.006));
        const float bTemp = static_cast<float>(std::pow(2.0, -m.temperature * 0.006));
        const float gTint = static_cast<float>(std::pow(2.0, m.tint * 0.006));
        const float dhBoost = 1.0f + static_cast<float>(m.dehaze) / 100.0f * 1.4f;
        constexpr float kHazeFloor = 0.12f;
        const float satScale = 1.0f + static_cast<float>(m.saturation) / 100.0f;
        const float clarityGain = static_cast<float>(m.clarity) / 100.0f * 0.45f;

        lrc::parallel::parallelFor(static_cast<size_t>(h), rowGrain,
                                   [&](size_t by, size_t ey) {
            for (int y = static_cast<int>(by); y < static_cast<int>(ey); ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    float wgt;
                    if (field) {
                        wgt = (*field)[i];
                        if (hasSubField)
                            wgt = m.maskUnion
                                      ? std::max(wgt, subField[i])
                                      : wgt * subField[i];
                        wgt *= opacity;
                    } else {
                        wgt = Pipeline::colorRangeWeight(
                                  m.color, base[i * 3], base[i * 3 + 1],
                                  base[i * 3 + 2]) * opacity;
                    }
                    if (wgt <= 1e-4f)
                        continue;
                float* px = p + i * 3;
                const float or0 = px[0], or1 = px[1], or2 = px[2];

                float r = or0, g = or1, b = or2;

                // Exposure (linear gain).
                r *= exposure;
                g *= exposure;
                b *= exposure;

                // Contrast around mid-gray.
                r = 0.18f + (r - 0.18f) * cscale;
                g = 0.18f + (g - 0.18f) * cscale;
                b = 0.18f + (b - 0.18f) * cscale;

                // Highlights / shadows (smooth zone adjustment).
                const float hw = smoothstep01((r - 0.35f) / 0.65f);
                const float hw_g = smoothstep01((g - 0.35f) / 0.65f);
                const float hw_b = smoothstep01((b - 0.35f) / 0.65f);
                r *= 1.0f + (highlightsGain - 1.0f) * hw;
                g *= 1.0f + (highlightsGain - 1.0f) * hw_g;
                b *= 1.0f + (highlightsGain - 1.0f) * hw_b;
                const float sw = smoothstep01((0.35f - r) / 0.35f);
                const float sw_g = smoothstep01((0.35f - g) / 0.35f);
                const float sw_b = smoothstep01((0.35f - b) / 0.35f);
                r *= 1.0f + (shadowsGain - 1.0f) * sw;
                g *= 1.0f + (shadowsGain - 1.0f) * sw_g;
                b *= 1.0f + (shadowsGain - 1.0f) * sw_b;

                // Whites & blacks.
                if (m.whites != 0.0 || m.blacks != 0.0) {
                    r += (r - 0.18f) * wGain * smoothstep01((r - 0.18f) / 0.82f);
                    g += (g - 0.18f) * wGain * smoothstep01((g - 0.18f) / 0.82f);
                    b += (b - 0.18f) * wGain * smoothstep01((b - 0.18f) / 0.82f);
                    r -= (0.18f - r) * bGain * smoothstep01((0.18f - r) / 0.18f);
                    g -= (0.18f - g) * bGain * smoothstep01((0.18f - g) / 0.18f);
                    b -= (0.18f - b) * bGain * smoothstep01((0.18f - b) / 0.18f);
                }

                // White balance.
                r *= rTemp;
                b *= bTemp;
                g *= gTint;

                // Dehaze.
                if (m.dehaze != 0.0) {
                    const float floor = kHazeFloor * (1.0f - dhBoost);
                    r = std::max(floor + (r - floor) * dhBoost, 0.0f);
                    g = std::max(floor + (g - floor) * dhBoost, 0.0f);
                    b = std::max(floor + (b - floor) * dhBoost, 0.0f);
                }

                // Saturation.
                if (m.saturation != 0.0) {
                    const float ls = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    r = ls + (r - ls) * satScale;
                    g = ls + (g - ls) * satScale;
                    b = ls + (b - ls) * satScale;
                }

                // Local clarity: high-pass luminance contrast, protected near
                // the tonal extremes to avoid halos.
                if (m.clarity != 0.0) {
                    const float L = luma[i];
                    const float protect =
                        smoothstep01(L / 0.05f) *
                        (1.0f - smoothstep01((L - 0.92f) / 0.08f));
                    const float d = (L - blurred[i]) * clarityGain * protect;
                    r += d;
                    g += d;
                    b += d;
                }

                // Blend with the original by the mask weight.
                px[0] = or0 + (r - or0) * wgt;
                px[1] = or1 + (g - or1) * wgt;
                px[2] = or2 + (b - or2) * wgt;
            }
        }
        });
    }
}

Pipeline::Pipeline(std::shared_ptr<const ColorManager> colorManager)
    : cm_(std::move(colorManager)),
      capacityBytes_(static_cast<size_t>(lrc::app::kCacheCapacityMB) * 1024u *
                     1024u) {}

void Pipeline::applyAdjustments(const Adjustments& a, float* p,
                                size_t pixelCount) {
    // HSL per-channel processing is active when any channel has a non-zero
    // adjustment. Hoisted out of the per-pixel loop so the check runs once
    // instead of once per pixel. activeMask is the bitset of such channels;
    // applyHSLPerChannel uses it to skip all-zero channels entirely.
    uint8_t activeMask = 0u;
    for (int i = 0; i < 8; ++i) {
        const HSLChannel& ch = a.hsl[i];
        if (ch.hue != 0.0 || ch.saturation != 0.0 || ch.luminance != 0.0)
            activeMask |= static_cast<uint8_t>(1u << i);
    }
    const bool useHsl = (activeMask != 0u);
    const bool useCurve = (a.curve.size() >= 2);

    // Fast path: every per-pixel parameter is neutral, so the stack is the
    // identity. The input is already normalized to [0,1]; returning without
    // touching it is exact (the per-pixel loop would only multiply by 1.0,
    // add 0.0 and clamp in-range values).
    if (a.exposure == 0.0 && a.contrast == 0.0 && a.highlights == 0.0 &&
        a.shadows == 0.0 && a.whites == 0.0 && a.blacks == 0.0 &&
        a.temperature == 0.0 && a.tint == 0.0 && a.dehaze == 0.0 &&
        a.vibrance == 0.0 && a.saturation == 0.0 && !useHsl && !useCurve &&
        a.colorTargets.empty() &&
        a.splitTone.highlightSaturation < 0.5 &&
        a.splitTone.shadowSaturation < 0.5)
        return;

    // The per-pixel stack (stages A-D). Splitting the original single
    // loop into per-stage passes keeps the incremental render path bit-identical
    // to the original output: each pass reproduces exactly the arithmetic of the
    // corresponding section, and stage D clamps once at the end as the original
    // loop did.
    applyStageA(a, p, pixelCount);
    applyStageB(a, p, pixelCount);
    applyStageC(a, p, pixelCount);
    applyStageD(a, p, pixelCount, activeMask);
}

void Pipeline::applySpatialAdjustments(const Adjustments& a, float* p,
                                       int w, int h) {
    // Full-resolution entry point: sigma scale 1.0 keeps every absolute-pixel
    // kernel at its authored size (the historical behaviour). The preview
    // path uses applySpatialAdjustmentsScaled with a preview/full scale.
    applySpatialAdjustmentsScaled(a, p, w, h, 1.0f);
}

void Pipeline::applySpotRemoval(const Adjustments& a, float* p, int w, int h) {
    if (!p || w <= 0 || h <= 0 || a.spotRemovals.empty())
        return;  // Neutral fast path: no spots leaves the buffer bit-exact.

    const float wf = static_cast<float>(w);
    const float hf = static_cast<float>(h);
    const float maxDim = std::max(wf, hf);
    // The inner 80% of the radius is fully opaque; the outer 20% is the
    // feathered transition band (alpha 1 -> 0) that softens the seam.
    constexpr float kFeatherInner = 0.8f;

    for (const SpotRemoval& s : a.spotRemovals) {
        if (!s.visible || s.radius <= 0.0f)
            continue;
        const float r = s.radius * maxDim;
        if (r < 0.5f)
            continue;  // sub-pixel radius: nothing to paint.

        const float tx = s.targetX * wf;
        const float ty = s.targetY * hf;
        const float sx = s.sourceX * wf;
        const float sy = s.sourceY * hf;

        // --- Snapshot the source bounding box --------------------------------
        // The source circle may overlap the target circle; sampling from the
        // live buffer while painting would read already-written pixels. A small
        // copy of the source bbox (pre-spot values) keeps the sample stable.
        const int regX0 =
            std::clamp(static_cast<int>(std::floor(sx - r)), 0, w - 1);
        const int regY0 =
            std::clamp(static_cast<int>(std::floor(sy - r)), 0, h - 1);
        const int regX1 =
            std::clamp(static_cast<int>(std::ceil(sx + r)), 0, w - 1);
        const int regY1 =
            std::clamp(static_cast<int>(std::ceil(sy + r)), 0, h - 1);
        const int regW = regX1 - regX0 + 1;
        const int regH = regY1 - regY0 + 1;
        if (regW <= 0 || regH <= 0)
            continue;
        ScratchBuffer srcBuf(static_cast<size_t>(regW) * regH * 3);
        float* src = srcBuf.data();
        for (int y = 0; y < regH; ++y) {
            const float* srow =
                p + (static_cast<size_t>(regY0 + y) * w + regX0) * 3;
            std::memcpy(src + static_cast<size_t>(y) * regW * 3, srow,
                        static_cast<size_t>(regW) * 3 * sizeof(float));
        }

        // --- Heal brightness match (Clone ignores it) ------------------------
        // Average Rec.709 luma over the [0.8r, r] annulus of the source rim
        // (from the snapshot) and the target rim (from the live buffer, which
        // is still pre-spot here). Scaling the source texture by
        // target/source makes the seam's brightness continuous.
        float scale = 1.0f;
        if (s.type == SpotType::Heal) {
            const float inner = kFeatherInner * r;
            const float r2 = r * r;
            const float in2 = inner * inner;
            double srcSum = 0.0, dstSum = 0.0;
            size_t srcCnt = 0, dstCnt = 0;

            const int sry0 = std::max(0, static_cast<int>(std::floor(sy - r)));
            const int sry1 = std::min(h - 1, static_cast<int>(std::ceil(sy + r)));
            const int srx0 = std::max(0, static_cast<int>(std::floor(sx - r)));
            const int srx1 = std::min(w - 1, static_cast<int>(std::ceil(sx + r)));
            for (int y = sry0; y <= sry1; ++y) {
                const int ssy = y - regY0;
                if (ssy < 0 || ssy >= regH)
                    continue;
                const float* srow = src + static_cast<size_t>(ssy) * regW * 3;
                for (int x = srx0; x <= srx1; ++x) {
                    const float dx = static_cast<float>(x) - sx;
                    const float dy = static_cast<float>(y) - sy;
                    const float d2 = dx * dx + dy * dy;
                    if (d2 < in2 || d2 > r2)
                        continue;
                    const int ssx = x - regX0;
                    if (ssx < 0 || ssx >= regW)
                        continue;
                    const float* px3 = srow + static_cast<size_t>(ssx) * 3;
                    srcSum += rec709Luma(px3[0], px3[1], px3[2]);
                    ++srcCnt;
                }
            }

            const int try0 = std::max(0, static_cast<int>(std::floor(ty - r)));
            const int try1 = std::min(h - 1, static_cast<int>(std::ceil(ty + r)));
            const int trx0 = std::max(0, static_cast<int>(std::floor(tx - r)));
            const int trx1 = std::min(w - 1, static_cast<int>(std::ceil(tx + r)));
            for (int y = try0; y <= try1; ++y) {
                const float* prow = p + (static_cast<size_t>(y) * w + trx0) * 3;
                for (int x = trx0; x <= trx1; ++x) {
                    const float dx = static_cast<float>(x) - tx;
                    const float dy = static_cast<float>(y) - ty;
                    const float d2 = dx * dx + dy * dy;
                    if (d2 < in2 || d2 > r2)
                        continue;
                    const float* px3 = prow + static_cast<size_t>(x - trx0) * 3;
                    dstSum += rec709Luma(px3[0], px3[1], px3[2]);
                    ++dstCnt;
                }
            }

            if (srcCnt > 0 && dstCnt > 0) {
                const float srcAvg =
                    static_cast<float>(srcSum / static_cast<double>(srcCnt));
                const float dstAvg =
                    static_cast<float>(dstSum / static_cast<double>(dstCnt));
                if (srcAvg > 1e-4f)
                    scale = dstAvg / srcAvg;
            }
            // Bounded to avoid pathological amplification/attenuation when one
            // rim is nearly black and the other bright.
            scale = std::clamp(scale, 0.1f, 10.0f);
        }

        // --- Paint the target circle -----------------------------------------
        // Iterate the target bounding box only (never the whole frame). Each
        // pixel is written independently, so the rows can be processed in
        // parallel without locks.
        const int bX0 = std::max(0, static_cast<int>(std::floor(tx - r)));
        const int bX1 = std::min(w - 1, static_cast<int>(std::ceil(tx + r)));
        const int bY0 = std::max(0, static_cast<int>(std::floor(ty - r)));
        const int bY1 = std::min(h - 1, static_cast<int>(std::ceil(ty + r)));
        if (bX1 < bX0 || bY1 < bY0)
            continue;
        const int bH = bY1 - bY0 + 1;
        const float r2 = r * r;
        const float inner = kFeatherInner * r;
        const float featherWidth = std::max((1.0f - kFeatherInner) * r, 1e-4f);
        const bool isHeal = (s.type == SpotType::Heal);

        const size_t rowGrain =
            lrc::parallel::defaultGrain(static_cast<size_t>(bH));
        lrc::parallel::parallelFor(static_cast<size_t>(bH), rowGrain,
                                   [&](size_t rowBegin, size_t rowEnd) {
            for (size_t ry = rowBegin; ry < rowEnd; ++ry) {
                const int y = bY0 + static_cast<int>(ry);
                const float dy = static_cast<float>(y) - ty;
                float* prow = p + (static_cast<size_t>(y) * w + bX0) * 3;
                for (int x = bX0; x <= bX1; ++x) {
                    const float dx = static_cast<float>(x) - tx;
                    const float d2 = dx * dx + dy * dy;
                    if (d2 > r2)
                        continue;
                    // The source sample for this target pixel uses the same
                    // offset from the source centre. Out-of-bounds sources are
                    // skipped: the pixel stays as it was (the Clone/Heal source
                    // may hang off the image edge).
                    const float sxs = sx + dx;
                    const float sys = sy + dy;
                    if (sxs < 0.0f || sxs >= wf || sys < 0.0f || sys >= hf)
                        continue;
                    const float ssx = sxs - static_cast<float>(regX0);
                    const float ssy = sys - static_cast<float>(regY0);

                    // Bilinear sample of the pre-spot source copy.
                    const float fx0 = std::floor(ssx), fy0 = std::floor(ssy);
                    const float fx = ssx - fx0, fy = ssy - fy0;
                    const int xi =
                        std::clamp(static_cast<int>(fx0), 0, regW - 1);
                    const int yi =
                        std::clamp(static_cast<int>(fy0), 0, regH - 1);
                    const int x2 = std::min(xi + 1, regW - 1);
                    const int y2 = std::min(yi + 1, regH - 1);
                    const float* r00 =
                        src + (static_cast<size_t>(yi) * regW + xi) * 3;
                    const float* r10 =
                        src + (static_cast<size_t>(yi) * regW + x2) * 3;
                    const float* r01 =
                        src + (static_cast<size_t>(y2) * regW + xi) * 3;
                    const float* r11 =
                        src + (static_cast<size_t>(y2) * regW + x2) * 3;

                    // Feather alpha: 1 inside 80% of the radius, 1 -> 0 across
                    // the outer 20% (smoothstep) so the seam is soft.
                    const float d = std::sqrt(d2);
                    float alpha = 1.0f;
                    if (d > inner) {
                        const float t = (d - inner) / featherWidth;
                        alpha = 1.0f - smoothstep01(std::clamp(t, 0.0f, 1.0f));
                    }

                    float* out = prow + static_cast<size_t>(x - bX0) * 3;
                    for (int c = 0; c < 3; ++c) {
                        const float top = r00[c] + (r10[c] - r00[c]) * fx;
                        const float bot = r01[c] + (r11[c] - r01[c]) * fx;
                        const float sampled = top + (bot - top) * fy;
                        const float healed = isHeal ? sampled * scale : sampled;
                        const float blended = out[c] + (healed - out[c]) * alpha;
                        out[c] = std::clamp(blended, 0.0f, 1.0f);
                    }
                }
            }
        });
    }
}

Pipeline::SpatialKernels Pipeline::spatialKernels(const Adjustments& a, int w,
                                                  int h, float sigmaScale) {
    SpatialKernels k;
    // Absolute-pixel kernels are scaled by the preview/full-resolution
    // factor so a preview keeps the same *image-space* radius as the export.
    k.textureSigma = 2.5f * sigmaScale;
    k.sharpnessSigma =
        std::clamp(static_cast<float>(a.sharpnessRadius), 0.4f, 4.0f) *
        sigmaScale;
    k.lumaNRSigma = std::clamp(
        0.5f + static_cast<float>(a.luminance) / 100.0f * 0.7f, 0.5f, 1.2f) *
        sigmaScale;
    k.lumaContrastSigma =
        std::clamp(
            1.5f + static_cast<float>(a.luminanceContrast) / 100.0f * 2.5f,
            1.5f, 4.0f) *
        sigmaScale;
    k.colorNRSigma =
        std::clamp(
            0.5f + static_cast<float>(a.colorNoiseReduction) / 100.0f *
                       (1.0f +
                        static_cast<float>(a.colorNoiseReductionSmoothness) /
                            100.0f * 1.5f),
            0.5f, 6.0f) *
        sigmaScale;
    k.grainCell =
        std::max(2.0f, 3.0f + static_cast<float>(a.grainSize) * 0.45f) *
        sigmaScale;
    // Buffer-relative kernel: derives from the working resolution, so it already
    // follows the same ratio and must NOT be scaled again.
    k.clarityRadius =
        std::max(4.0f, static_cast<float>(std::min(w, h)) / 50.0f);
    return k;
}

float Pipeline::previewSigmaScale(int workingLongEdge, int fullLongEdge) {
    if (workingLongEdge <= 0 || fullLongEdge <= 0)
        return 1.0f;
    if (workingLongEdge >= fullLongEdge)
        return 1.0f;  // never upscale kernels
    return static_cast<float>(workingLongEdge) /
           static_cast<float>(fullLongEdge);
}

ImageBufferPtr Pipeline::renderPreview(const ImageBuffer& source,
                                       const Adjustments& adjustments,
                                       const Size& targetSize,
                                       int fullLongEdge, uint64_t imageId,
                                       ImageBufferPtr* linearDisplayOut) {
    if (source.empty() || targetSize.width <= 0 || targetSize.height <= 0)
        return {};
    std::lock_guard<std::mutex> lock(renderStateMutex_);

    // --- 1. Downscale (contain-fit, never upscale) into an F32 working copy.
    const double scale = std::min(
        1.0,
        std::min(static_cast<double>(targetSize.width) / source.width(),
                 static_cast<double>(targetSize.height) / source.height()));
    const int nw =
        std::max(1, static_cast<int>(std::lround(source.width() * scale)));
    const int nh =
        std::max(1, static_cast<int>(std::lround(source.height() * scale)));
    ImageBufferPtr working;
    if (nw == source.width() && nh == source.height()) {
        working = (source.format() == PixelFormat::F32)
                      ? std::make_shared<ImageBuffer>(source)
                      : source.toF32();
    } else {
        working = (source.format() == PixelFormat::F32)
                      ? source.resize(nw, nh)
                      : source.resizeToF32(nw, nh);
    }
    if (!working || working->empty())
        return {};
    const int refLong = (fullLongEdge > 0)
                            ? fullLongEdge
                            : std::max(source.width(), source.height());
    const int workingLong = std::max(nw, nh);
    const float sigmaScale = previewSigmaScale(workingLong, refLong);

    // --- 2. Per-stage hashes of the incoming adjustment stack.
    const uint64_t stageHashes[6] = {
        static_cast<uint64_t>(adjustments.hashStageA()),
        static_cast<uint64_t>(adjustments.hashStageB()),
        static_cast<uint64_t>(adjustments.hashStageC()),
        static_cast<uint64_t>(adjustments.hashStageD()),
        static_cast<uint64_t>(adjustments.hashStageE()),
        static_cast<uint64_t>(Adjustments::hashStageF()),
    };

    // --- 3. Cache identity (image + working resolution + content).
    const uint64_t sourceKey =
        computeSourceKey(source, nw, nh, refLong, imageId);

    // --- 4. Find the first changed stage (or serve entirely from cache).
    int firstChanged = 0;
    if (renderState_.valid && renderState_.sourceKey == sourceKey) {
        firstChanged = -1;
        for (int i = 0; i < 6; ++i) {
            if (stageHashes[i] != renderState_.stageHashes[i]) {
                firstChanged = i;
                break;
            }
        }
        if (firstChanged < 0) {
            // Nothing changed: serve the cached final frame directly.
            lastRecomputeStage_ = -1;
            lastReuseStartStage_ = -1;
            if (renderState_.stages[5])
                return renderState_.stages[5];
            firstChanged = 0;
        }
    }

    // --- 5. Actual restart stage: move forward to the first missing retained
    // stage below the first change (an evicted intermediate forces a restart
    // from that point).
    int start = firstChanged;
    for (int i = 0; i < firstChanged; ++i) {
        if (!renderState_.present[i]) {
            start = i;
            break;
        }
    }

    // LRU bookkeeping: reused stages count as accessed.
    for (int i = 0; i < start; ++i)
        touchStageLocked(i);

    // --- 6. Prepare the writable input for the restart stage.
    ImageBufferPtr input;
    if (start == 0) {
        input = working;  // the resized source copy; `source` is untouched.
    } else {
        input = std::make_shared<ImageBuffer>(*renderState_.stages[start - 1]);
    }

    // --- 7. Run stages start..5, retaining A-D + F (E is never retained).
    const size_t pixelCount = static_cast<size_t>(nw) * nh;

    // Per-render bitset of active HSL channels for stage D. Computed once here
    // (instead of per-pixel) and passed through to applyStageD/applyHSLPerChannel.
    uint8_t activeMask = 0u;
    for (int i = 0; i < 8; ++i) {
        const HSLChannel& ch = adjustments.hsl[i];
        if (ch.hue != 0.0 || ch.saturation != 0.0 || ch.luminance != 0.0)
            activeMask |= static_cast<uint8_t>(1u << i);
    }

    for (int i = start; i <= 5; ++i) {
        if (i == 5) {
            // Stage F: sRGB output. input holds the stage-E output (a writable
            // copy that stage E consumed); toDisplaySRGB only reads it.
            // Hand the adjusted linear F32 display buffer (the stage-F input)
            // to the caller before the sRGB conversion so a linear-light
            // histogram can be derived without a second render pass.
            if (linearDisplayOut)
                *linearDisplayOut = input;
            ImageBufferPtr u8 = cm_->toDisplaySRGB(*input);
            if (renderState_.present[5])
                renderState_.bytes -= renderState_.stages[5]->sizeBytes();
            renderState_.stages[5] = u8;
            renderState_.present[5] = true;
            renderState_.stageHashes[5] = stageHashes[5];
            renderState_.bytes += u8->sizeBytes();
            touchStageLocked(5);
            break;
        }
        float* px = reinterpret_cast<float*>(input->data().data());
        switch (i) {
            case 0: applyStageA(adjustments, px, pixelCount); break;
            case 1: applyStageB(adjustments, px, pixelCount); break;
            case 2: applyStageC(adjustments, px, pixelCount); break;
            case 3: applyStageD(adjustments, px, pixelCount, activeMask); break;
            case 4:
                applySpatialAdjustmentsScaled(adjustments, px, nw, nh,
                                              sigmaScale);
                break;
            default: break;
        }
        if (i < 4) {
            // Retain this stage output (F32) for future incremental reuse.
            if (renderState_.present[i])
                renderState_.bytes -= renderState_.stages[i]->sizeBytes();
            renderState_.stages[i] = input;
            renderState_.present[i] = true;
            renderState_.stageHashes[i] = stageHashes[i];
            renderState_.bytes += input->sizeBytes();
            touchStageLocked(i);
            // The retained node is immutable once cached; the next stage needs
            // its own writable copy.
            input = std::make_shared<ImageBuffer>(*renderState_.stages[i]);
        } else {
            // Stage E output is deliberately NOT retained: nothing downstream
            // reads it (F always recomputes after E), so caching it would only
            // consume the memory budget. Keep the hash so the "nothing changed"
            // fast path still detects an E-parameter change.
            renderState_.stageHashes[4] = stageHashes[4];
            renderState_.present[4] = false;
            renderState_.stages[4].reset();
        }
    }

    renderState_.sourceKey = sourceKey;
    renderState_.valid = true;
    evictIncrementalLocked();

    lastRecomputeStage_ = start;
    lastReuseStartStage_ = (start > 0) ? start - 1 : -1;
    return renderState_.stages[5];
}

void Pipeline::clearIncrementalCache() {
    std::lock_guard<std::mutex> lock(renderStateMutex_);
    renderState_ = RenderState{};
    lastRecomputeStage_ = -1;
    lastReuseStartStage_ = -1;
}

void Pipeline::setIncrementalCacheCapacity(size_t capacityBytes) {
    std::lock_guard<std::mutex> lock(renderStateMutex_);
    incrementalBudgetBytes_ = capacityBytes;
    evictIncrementalLocked();
}

size_t Pipeline::incrementalCacheCapacity() const {
    std::lock_guard<std::mutex> lock(renderStateMutex_);
    return incrementalBudgetBytes_;
}

size_t Pipeline::incrementalCacheBytes() const {
    std::lock_guard<std::mutex> lock(renderStateMutex_);
    return renderState_.bytes;
}

int Pipeline::lastRecomputeStage() const {
    std::lock_guard<std::mutex> lock(renderStateMutex_);
    return lastRecomputeStage_;
}

int Pipeline::lastReuseStartStage() const {
    std::lock_guard<std::mutex> lock(renderStateMutex_);
    return lastReuseStartStage_;
}

void Pipeline::dropIncrementalStageForTest(int stage) {
    // Test-only: deliberately release a single retained intermediate so the
    // renderPreview restart heuristic can be exercised. Only stages 0..3 are
    // retained (A-D; stage E is never kept) so stage 4 is a no-op, matching
    // real eviction behaviour.
    std::lock_guard<std::mutex> lock(renderStateMutex_);
    if (stage < 0 || stage > 3)
        return;
    if (!renderState_.present[stage])
        return;
    renderState_.bytes -= renderState_.stages[stage]->sizeBytes();
    renderState_.stages[stage].reset();
    renderState_.present[stage] = false;
    // Invalidate the hash so a subsequent render that would otherwise think
    // it's still valid will correctly fall back to rebuilding.
    renderState_.stageHashes[stage] = 0;
}

void Pipeline::touchStageLocked(int stage) {
    renderState_.accessTick[stage] = ++renderState_.tick;
}

void Pipeline::evictIncrementalLocked() {
    while (renderState_.bytes > incrementalBudgetBytes_) {
        // Evict the least-recently-touched retained stage, never stage F (the
        // final output, kept for the "nothing changed" fast path and the UI's
        // last-render read).
        int victim = -1;
        uint32_t minTick = 0xFFFFFFFFu;
        for (int i = 0; i < 5; ++i) {
            if (renderState_.present[i] && renderState_.accessTick[i] < minTick) {
                minTick = renderState_.accessTick[i];
                victim = i;
            }
        }
        if (victim < 0)
            break;
        renderState_.bytes -= renderState_.stages[victim]->sizeBytes();
        renderState_.stages[victim].reset();
        renderState_.present[victim] = false;
    }
}

uint64_t Pipeline::computeSourceKey(const ImageBuffer& source, int workingW,
                                    int workingH, int fullLongEdge,
                                    uint64_t imageId) const {
    size_t h = 1469598103934665603ull;
    h = hashCombine(h, static_cast<size_t>(imageId));
    h = hashCombine(h, static_cast<size_t>(static_cast<uint32_t>(workingW)));
    h = hashCombine(h, static_cast<size_t>(static_cast<uint32_t>(workingH)));
    h = hashCombine(h, static_cast<size_t>(
                           static_cast<uint32_t>(std::max(fullLongEdge, 0))));
    // Content fingerprint: hash a strided sample so a different image with the
    // same dimensions (or the same buffer reallocated at a new address) is never
    // served stale stage outputs.
    const std::vector<uint8_t>& bytes = source.data();
    const size_t n = bytes.size();
    const size_t stride = std::max<size_t>(1, n / 4096);
    for (size_t i = 0; i < n; i += stride)
        h = hashCombine(h, static_cast<size_t>(bytes[i]));
    return static_cast<uint64_t>(h);
}

Adjustments Pipeline::autoAdjust(const ImageBuffer& linear) {
    Adjustments out;
    if (linear.empty())
        return out;
    ImageBufferPtr f32 = (linear.format() == PixelFormat::F32)
                             ? std::make_shared<ImageBuffer>(linear)
                             : linear.toF32();
    const float* d = reinterpret_cast<const float*>(f32->data().data());
    const size_t n = static_cast<size_t>(f32->width()) * f32->height();
    if (n == 0)
        return out;

    double sum = 0.0;
    double sumSat = 0.0;
    int hist[256] = {};
    for (size_t i = 0; i < n; ++i) {
        const float luma = rec709Luma(d[i * 3], d[i * 3 + 1], d[i * 3 + 2]);
        sum += luma;
        hist[std::clamp(static_cast<int>(luma * 255.0f), 0, 255)]++;
        const float mn = std::min({d[i * 3], d[i * 3 + 1], d[i * 3 + 2]});
        const float mx = std::max({d[i * 3], d[i * 3 + 1], d[i * 3 + 2]});
        sumSat += (mx > 1e-6f) ? (mx - mn) / mx : 0.0f;
    }
    const double mean = sum / static_cast<double>(n);
    const double meanSat = sumSat / static_cast<double>(n);

    // 5th / 95th luminance percentiles for a contrast estimate.
    const size_t p5n = std::max<size_t>(1, static_cast<size_t>(0.05 * n));
    const size_t p95n = std::max<size_t>(1, static_cast<size_t>(0.95 * n));
    size_t acc = 0;
    int lo = 0;
    for (int bin = 0; bin < 256; ++bin) {
        acc += static_cast<size_t>(hist[bin]);
        if (acc >= p5n) { lo = bin; break; }
    }
    acc = 0;
    int hi = 255;
    for (int bin = 0; bin < 256; ++bin) {
        acc += static_cast<size_t>(hist[bin]);
        if (acc >= p95n) { hi = bin; break; }
    }
    const double span = (hi - lo) / 255.0;

    // Pass 1: exposure / contrast / vibrance. A dead-zone keeps images that
    // are already close to neutral from picking up a tiny correction.

    // Exposure: pull the mean luminance toward mid-gray (0.18), but leave
    // corrections under ~0.05 stops untouched.
    const double desiredExp =
        std::clamp(std::log2(0.18 / std::max(mean, 1e-6)), -3.0, 3.0);
    if (std::abs(desiredExp) >= 0.05)
        out.exposure = desiredExp;
    // Contrast: compressed tonal span gets a boost, blown-out span is tamed.
    const double desiredCont = std::clamp((0.45 - span) * 140.0, -50.0, 50.0);
    if (std::abs(desiredCont) >= 2.0)
        out.contrast = desiredCont;
    // Vibrance: dull images get a gentle saturation lift.
    if (meanSat < 0.35)
        out.vibrance = std::clamp((0.35 - meanSat) * 200.0, 0.0, 60.0);

    // Pass 2: highlights / shadows / whites / blacks on the exposure-corrected
    // luma, plus grey-band auto white balance in the original pixel domain.
    const float gain = static_cast<float>(std::pow(2.0, out.exposure));
    int histC[256] = {};
    size_t hiCnt = 0;   // pixels whose corrected luma exceeds 0.95
    size_t loCnt = 0;   // pixels whose corrected luma falls below 0.02
    double mr = 0.0;    // grey-band (p5..p95) channel means, original domain
    double mg = 0.0;
    double mb = 0.0;
    size_t awbCnt = 0;
    for (size_t i = 0; i < n; ++i) {
        const float luma = rec709Luma(d[i * 3], d[i * 3 + 1], d[i * 3 + 2]);
        const float lumaC = std::clamp(luma * gain, 0.0f, 1.0f);
        histC[std::clamp(static_cast<int>(lumaC * 255.0f), 0, 255)]++;
        if (lumaC > 0.95f)
            ++hiCnt;
        if (lumaC < 0.02f)
            ++loCnt;
        const int bin = std::clamp(static_cast<int>(luma * 255.0f), 0, 255);
        if (bin >= lo && bin <= hi) {
            mr += d[i * 3];
            mg += d[i * 3 + 1];
            mb += d[i * 3 + 2];
            ++awbCnt;
        }
    }
    const double hiFrac = static_cast<double>(hiCnt) / static_cast<double>(n);
    const double loFrac = static_cast<double>(loCnt) / static_cast<double>(n);

    // 99th / 1st corrected-luma percentiles from the corrected histogram.
    const size_t p99n = std::max<size_t>(1, static_cast<size_t>(0.99 * n));
    const size_t p1n = std::max<size_t>(1, static_cast<size_t>(0.01 * n));
    double l99 = 1.0;
    acc = 0;
    for (int bin = 0; bin < 256; ++bin) {
        acc += static_cast<size_t>(histC[bin]);
        if (acc >= p99n) { l99 = bin / 255.0; break; }
    }
    double l1 = 0.0;
    acc = 0;
    for (int bin = 0; bin < 256; ++bin) {
        acc += static_cast<size_t>(histC[bin]);
        if (acc >= p1n) { l1 = bin / 255.0; break; }
    }

    // Highlights: only ever pull blown-out highlights back down.
    if (hiFrac > 0.03)
        out.highlights = std::clamp((hiFrac - 0.03) * -180.0, -100.0, 0.0);
    // Shadows: lift crushed shadows toward the mid-tones.
    if (loFrac > 0.03)
        out.shadows = std::clamp((loFrac - 0.03) * 180.0, 0.0, 100.0);
    // Whites: if the 99th percentile sits below 0.90, raise the white point.
    if (l99 < 0.90)
        out.whites = std::clamp((0.90 - l99) * 200.0, 0.0, 100.0);
    // Blacks: if the 1st percentile is lifted off zero, push the black point.
    if (l1 > 0.04)
        out.blacks = std::clamp((l1 - 0.04) * 200.0, 0.0, 100.0);

    // Auto white balance from the grey-band means, with a 2-point dead-zone so
    // a near-neutral cast is left alone. estimateWhiteBalance already clamps to
    // the -100..+100 slider domain.
    if (awbCnt > 0) {
        const double inv = 1.0 / static_cast<double>(awbCnt);
        const WhiteBalanceEstimate est = estimateWhiteBalance(
            static_cast<float>(mr * inv), static_cast<float>(mg * inv),
            static_cast<float>(mb * inv));
        if (std::abs(est.temperature) > 2.0)
            out.temperature = std::clamp(est.temperature, -100.0, 100.0);
        if (std::abs(est.tint) > 2.0)
            out.tint = std::clamp(est.tint, -100.0, 100.0);
    }

    // Optional auto tone curve: a flat tonal span gets a gentle S-curve whose
    // zone outputs all stay inside the anchor range [0.25..0.82].
    const double s = std::clamp((0.45 - span) * 1.5, 0.0, 1.0) * 0.06;
    if (s > 0.01)
        out.curve = zonecurve::fromOutputs(
            static_cast<float>(0.25 + s), static_cast<float>(0.47 + 0.5 * s),
            static_cast<float>(0.64 - 0.5 * s), static_cast<float>(0.82 - s));

    return out;
}

Pipeline::WhiteBalanceEstimate Pipeline::estimateWhiteBalance(float lr, float lg,
                                                              float lb) {
    WhiteBalanceEstimate e;
    // Grey-world guard: a null or near-black channel carries no reliable colour
    // ratio, so report "no adjustment" instead of dividing by ~0.
    constexpr double kMinChannel = 1e-4;
    if (lr <= kMinChannel || lg <= kMinChannel || lb <= kMinChannel)
        return e;

    // Pipeline white-balance gains (see applyAdjustments / applyLocalAdjustments):
    //   R' = lr * 2^(t*0.006)
    //   B' = lb * 2^(-t*0.006)
    //   G' = lg * 2^(tint*0.006)
    // Solve for the (temperature t, tint) that makes R' == G' == B' so the
    // sample becomes neutral grey.

    // Temperature from R' == B':
    //   lr * 2^(t*0.006) == lb * 2^(-t*0.006)
    //   lr * 2^(t*0.012) == lb            (multiply both sides by 2^(t*0.006))
    //   t = log2(lb / lr) / 0.012
    const double temperature = std::log2(static_cast<double>(lb) / lr) / 0.012;

    // Tint from R' == G' (temperature does not touch G; tint only scales G):
    //   lr * 2^(t*0.006) == lg * 2^(tint*0.006)
    //   2^(tint*0.006)   == (lr / lg) * 2^(t*0.006)
    //   tint             == log2(lr / lg) / 0.006 + t
    const double tint =
        std::log2(static_cast<double>(lr) / lg) / 0.006 + temperature;

    // Clamp to the Adjustments temperature/tint slider domain (-100..+100) so a
    // single extreme pixel cannot push the estimate past what the develop panel
    // can express, and to keep the returned value directly usable as an offset.
    constexpr double kLimit = 100.0;
    e.temperature = std::clamp(temperature, -kLimit, kLimit);
    e.tint = std::clamp(tint, -kLimit, kLimit);
    return e;
}

ImageBufferPtr Pipeline::visualizeOverlay(const ImageBuffer& linear,
                                          const Adjustments& adjustments,
                                          VisualizeMode mode, int maxDim) const {
    ImageBufferPtr working;
    if (maxDim > 0) {
        const int longest = std::max(linear.width(), linear.height());
        if (longest > maxDim) {
            const double s = static_cast<double>(maxDim) / longest;
            const int nw = std::max(1, static_cast<int>(linear.width() * s));
            const int nh = std::max(1, static_cast<int>(linear.height() * s));
            // Keep the resize in float space (see renderInternalLinear) so an
            // integer source never round-trips through a quantized U16 copy.
            working = (linear.format() == PixelFormat::F32)
                          ? linear.resize(nw, nh)
                          : linear.resizeToF32(nw, nh);
        } else {
            working = std::make_shared<ImageBuffer>(linear);
        }
    } else {
        working = std::make_shared<ImageBuffer>(linear);
    }
    ImageBufferPtr f32 = (working->format() == PixelFormat::F32)
                             ? working
                             : working->toF32();
    const int w = f32->width();
    const int h = f32->height();
    const size_t n = static_cast<size_t>(w) * h;
    if (n == 0 || w <= 0 || h <= 0)
        return {};

    auto out = ImageBuffer::create(w, h, PixelFormat::U8);
    uint8_t* dst = out->data().data();

    switch (mode) {
        case VisualizeMode::Clipping: {
            // Render through the full pipeline (linear light) and convert to
            // display sRGB, then paint a grayscale image with red = clipped
            // highlights and blue = clipped shadows (LR's Alt+Exposure view).
            ImageBufferPtr rendered = renderInternalLinear(linear, adjustments, maxDim);
            ImageBufferPtr sRGB = cm_->toDisplaySRGB(*rendered);
            const uint8_t* src = sRGB->data().data();
            for (size_t i = 0; i < n; ++i) {
                const uint8_t r = src[i * 3 + 0];
                const uint8_t g = src[i * 3 + 1];
                const uint8_t b = src[i * 3 + 2];
                const uint8_t luma = static_cast<uint8_t>(
                    0.299f * r + 0.587f * g + 0.114f * b);
                const bool hiClip = (r >= 252 && g >= 252 && b >= 252);
                const bool loClip = (r <= 4 && g <= 4 && b <= 4);
                if (hiClip) {
                    dst[i * 3 + 0] = 255; dst[i * 3 + 1] = 0; dst[i * 3 + 2] = 0;
                } else if (loClip) {
                    dst[i * 3 + 0] = 0; dst[i * 3 + 1] = 0; dst[i * 3 + 2] = 255;
                } else {
                    dst[i * 3 + 0] = luma; dst[i * 3 + 1] = luma; dst[i * 3 + 2] = luma;
                }
            }
            break;
        }
        case VisualizeMode::DetailMask: {
            // B&W map of the mid-frequency detail the clarity/texture/dehaze
            // sliders act on: luma minus a gaussian base, centred on mid-grey.
            const float* data = reinterpret_cast<const float*>(f32->data().data());
            ScratchBuffer luma(n);
            lumaImage(data, w, h, luma.vec());
            const float radius =
                std::max(4.0f, static_cast<float>(std::min(w, h)) / 50.0f);
            ScratchBuffer blurred(n);
            gaussianBlur(luma.data(), w, h, radius, blurred.vec());
            for (size_t i = 0; i < n; ++i) {
                const float detail = luma[i] - blurred[i];
                const float v =
                    std::clamp(0.5f + detail * 8.0f, 0.0f, 1.0f);
                const uint8_t c = static_cast<uint8_t>(v * 255.0f);
                dst[i * 3 + 0] = c; dst[i * 3 + 1] = c; dst[i * 3 + 2] = c;
            }
            break;
        }
        case VisualizeMode::EdgeMask: {
            // Sobel edge magnitude of the linear luma, thresholded by the
            // sharpening masking slider: white = edges that will be sharpened.
            const float* data = reinterpret_cast<const float*>(f32->data().data());
            ScratchBuffer luma(n);
            lumaImage(data, w, h, luma.vec());
            std::vector<float> edges(n, 0.0f);
            float maxEdge = 1e-6f;
            for (int y = 1; y < h - 1; ++y) {
                for (int x = 1; x < w - 1; ++x) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    const float gx = -luma[i - w - 1] + luma[i - w + 1] -
                                     2.0f * luma[i - 1] + 2.0f * luma[i + 1] -
                                     luma[i + w - 1] + luma[i + w + 1];
                    const float gy = -luma[i - w - 1] - 2.0f * luma[i - w] -
                                     luma[i - w + 1] + luma[i + w - 1] +
                                     2.0f * luma[i + w] + luma[i + w + 1];
                    edges[i] = std::sqrt(gx * gx + gy * gy);
                    maxEdge = std::max(maxEdge, edges[i]);
                }
            }
            const float maskThresh =
                static_cast<float>(adjustments.sharpnessMasking) / 100.0f;
            // Higher masking = higher edge threshold = fewer white pixels.
            const float cutoff = 0.02f + maskThresh * 0.5f;
            for (size_t i = 0; i < n; ++i) {
                const float e = edges[i] / maxEdge;
                const uint8_t c = (e > cutoff) ? 255 : 0;
                dst[i * 3 + 0] = c; dst[i * 3 + 1] = c; dst[i * 3 + 2] = c;
            }
            break;
        }
    }
    return out;
}

ImageBufferPtr Pipeline::renderInternalLinear(const ImageBuffer& linear,
                                              const Adjustments& adjustments,
                                              int maxDim) const {
    ImageBufferPtr f32;
    if (maxDim > 0) {
        const int longest = std::max(linear.width(), linear.height());
        if (longest > maxDim) {
            const double s = static_cast<double>(maxDim) / longest;
            const int nw = std::max(1, static_cast<int>(linear.width() * s));
            const int nh = std::max(1, static_cast<int>(linear.height() * s));
            // Integer sources resize straight into F32: a same-type (U16)
            // resize would quantize the internal F32 pass to 16 bit and then
            // be converted back here -- a precision-losing F32->U16->F32
            // roundtrip on every preview frame.
            f32 = (linear.format() == PixelFormat::F32)
                      ? linear.resize(nw, nh)
                      : linear.resizeToF32(nw, nh);
        } else if (linear.format() == PixelFormat::F32) {
            // Same size, already float: must copy -- applyAdjustments edits in
            // place and the caller's buffer must stay untouched.
            f32 = std::make_shared<ImageBuffer>(linear);
        } else {
            // Same size, integer source: convert straight to F32 and skip the
            // throwaway U16/U8 intermediate copy.
            f32 = linear.toF32();
        }
    } else if (linear.format() == PixelFormat::F32) {
        // Full resolution, already float: copy (in-place edit below).
        f32 = std::make_shared<ImageBuffer>(linear);
    } else {
        // Full resolution, integer source: direct F32 conversion.
        f32 = linear.toF32();
    }

    const int w = f32->width();
    const int h = f32->height();
    const size_t pixelCount = static_cast<size_t>(w) * h;
    float* data = reinterpret_cast<float*>(f32->data().data());
    applyAdjustments(adjustments, data, pixelCount);
    // Spatial stages need the neighbourhood, so they run after the per-pixel
    // stack (and stay in linear light).
    applySpatialAdjustments(adjustments, data, w, h);
    return f32;
}

ImageBufferPtr Pipeline::renderInternal(const ImageBuffer& linear,
                                        const Adjustments& adjustments,
                                        int maxDim) const {
    ImageBufferPtr f32 = renderInternalLinear(linear, adjustments, maxDim);
    return cm_->toDisplaySRGB(*f32);
}

bool Pipeline::lookupCache(const CacheKey& key, ImageBufferPtr& out) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    auto it = cache_.find(key);
    if (it == cache_.end())
        return false;
    // Move to the front of the LRU list.
    lru_.remove(key);
    lru_.push_front(key);
    out = it->second;
    return true;
}

void Pipeline::insertCache(const CacheKey& key, const ImageBufferPtr& buffer) {
    if (!buffer || buffer->empty())
        return;
    const size_t size = buffer->sizeBytes();
    std::lock_guard<std::mutex> lock(cacheMutex_);
    lru_.remove(key);
    cache_[key] = buffer;
    cacheBytes_ += size;
    lru_.push_front(key);
    // Evict least-recently-used entries while over capacity.
    while (cacheBytes_ > capacityBytes_ && lru_.size() > 1) {
        const CacheKey victim = lru_.back();
        lru_.pop_back();
        auto vit = cache_.find(victim);
        if (vit != cache_.end()) {
            cacheBytes_ -= vit->second->sizeBytes();
            cache_.erase(vit);
        }
    }
}

void Pipeline::clearCache() {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    cache_.clear();
    lru_.clear();
    cacheBytes_ = 0;
}

void Pipeline::setCacheCapacity(size_t capacityBytes) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    capacityBytes_ = capacityBytes;
}

size_t Pipeline::cacheCapacityBytes() const {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    return capacityBytes_;
}

size_t Pipeline::cacheBytes() const {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    return cacheBytes_;
}

ImageBufferPtr Pipeline::render(uint64_t imageId, const ImageBuffer& linear,
                                const Adjustments& adjustments, int maxDim) {
    const CacheKey key{imageId, adjustments.hash(), maxDim};
    ImageBufferPtr cached;
    if (lookupCache(key, cached))
        return cached;
    ImageBufferPtr result = renderInternal(linear, adjustments, maxDim);
    insertCache(key, result);
    return result;
}

ImageBufferPtr Pipeline::renderFull(uint64_t imageId, const ImageBuffer& linear,
                                    const Adjustments& adjustments) {
    return render(imageId, linear, adjustments, 0);
}

ImageBufferPtr Pipeline::renderLinearFull(uint64_t imageId,
                                          const ImageBuffer& linear,
                                          const Adjustments& adjustments) {
    // Deliberately not cached: a full-resolution linear buffer is far too large
    // for the preview LRU and would evict every thumbnail. Export runs are
    // one-shot by nature. `imageId` is kept in the signature for symmetry.
    (void)imageId;
    return renderInternalLinear(linear, adjustments, 0);
}

ImageBufferPtr Pipeline::renderFullDisplay(uint64_t imageId,
                                           const ImageBuffer& linear,
                                           const Adjustments& adjustments) {
    // Full-resolution linear render followed by the display sRGB conversion.
    // Deliberately not cached (see renderLinearFull): a full-size U8 buffer
    // would evict every preview from the LRU, and 1:1 renders are one-shot per
    // image. `imageId` is kept in the signature for symmetry with the cached
    // paths.
    (void)imageId;
    ImageBufferPtr f32 = renderInternalLinear(linear, adjustments, 0);
    return cm_->toDisplaySRGB(*f32);
}

// --- AI inference injection --------------------------------------------------
// Static engine pointer (definition). Initialized to null at static-storage
// init time; set once at app startup by MainWindow after the bundled models
// have been loaded. See Pipeline.h for the threading / lifetime contract.
AiInferenceEngine* Pipeline::aiEngine_ = nullptr;

AiInferenceEngine* Pipeline::globalAiEngine() { return aiEngine_; }

void Pipeline::setGlobalAiEngine(AiInferenceEngine* engine) {
    aiEngine_ = engine;
}

} // namespace lrc
