#include "engine/HdrMerge.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace lrc {

namespace {

constexpr float kRefLuma = 0.18f; // mid-gray in linear light

// Rec.709 luma of a linear RGB pixel.
inline float luma(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// Well-exposedness weight: 1 near mid-gray, falling off toward black and
// clipping in log-luminance space (sigma in stops).
inline float wellExposed(float L) {
    const float lg = std::log2(std::max(L, 1e-5f) / kRefLuma);
    return std::exp(-0.5f * (lg / 1.0f) * (lg / 1.0f));
}

// Median of a sorted channel list across frames (robust against ghosts).
inline float medianOf(std::vector<float>& vals) {
    std::sort(vals.begin(), vals.end());
    const size_t n = vals.size();
    if (n == 0)
        return 0.0f;
    if (n % 2 == 1)
        return vals[n / 2];
    return 0.5f * (vals[n / 2 - 1] + vals[n / 2]);
}

} // namespace

std::vector<float> HdrMerge::lumaSmall(const ImageBuffer& buf, int maxDim,
                                       int& w, int& h) {
    const int srcW = buf.width();
    const int srcH = buf.height();
    const int longest = std::max(srcW, srcH);
    const double s = (longest > maxDim)
                         ? static_cast<double>(maxDim) / longest
                         : 1.0;
    w = std::max(1, static_cast<int>(std::lround(srcW * s)));
    h = std::max(1, static_cast<int>(std::lround(srcH * s)));
    std::vector<float> out(static_cast<size_t>(w) * h, 0.0f);

    // Resolving the format once here keeps the sampling loops free of the
    // per-component bounds check and format switch that pixel() performs on
    // every access. U16/U8 sources keep the normalized pixel() path at the end
    // of this function -- that branch is taken once, never inside the loop.
    const std::span<const float> fast = buf.asFloatSpan();
    const size_t stride = static_cast<size_t>(buf.channels());

    // Area-average downscale of the luminance.
    if (!fast.empty()) {
        // --- typed F32 path ---
        for (int y = 0; y < h; ++y) {
            const int y0 = static_cast<int>(y / s);
            const int y1 = std::max(y0, static_cast<int>((y + 1) / s) - 1);
            // Rounding w/h can push the last target cell's sample window a
            // little past the source edge. pixel() reported such coordinates
            // as 0.0f while still counting them, so only the valid
            // sub-rectangle is accumulated and the original sample count is
            // kept below: the clipped samples contribute exactly +0.0, which
            // is what the count-only arithmetic reproduces.
            const int syHi = std::min(y1, srcH - 1);
            for (int x = 0; x < w; ++x) {
                const int x0 = static_cast<int>(x / s);
                const int x1 = std::max(x0, static_cast<int>((x + 1) / s) - 1);
                const int sxHi = std::min(x1, srcW - 1);
                const long total = static_cast<long>(x1 - x0 + 1) *
                                   static_cast<long>(y1 - y0 + 1);
                double acc = 0.0;
                for (int sy = y0; sy <= syHi; ++sy) {
                    const float* row =
                        fast.data() + static_cast<size_t>(sy) * srcW * stride;
                    for (int sx = x0; sx <= sxHi; ++sx) {
                        const float* p = row + static_cast<size_t>(sx) * stride;
                        // Same expression shape and evaluation order as the
                        // pixel() path: the literals are double, so the
                        // accumulation stays in double -- 0.2126 first, then
                        // + 0.7152, then + 0.0722. Do not reorder or narrow.
                        acc += p[0] * 0.2126 + p[1] * 0.7152 + p[2] * 0.0722;
                    }
                }
                if (total > static_cast<long>(sxHi - x0 + 1) *
                                static_cast<long>(syHi - y0 + 1)) {
                    // Every clipped sample used to add three +0.0 terms; a
                    // single +0.0 addition reproduces the only observable
                    // effect (a -0.0 sum becoming +0.0).
                    acc += 0.0;
                }
                out[static_cast<size_t>(y) * w + x] =
                    total > 0 ? static_cast<float>(acc / total) : 0.0f;
            }
        }
        return out;
    }

    // --- normalized fallback for U16/U8 sources ---
    for (int y = 0; y < h; ++y) {
        const int y0 = static_cast<int>(y / s);
        const int y1 = std::max(y0, static_cast<int>((y + 1) / s) - 1);
        for (int x = 0; x < w; ++x) {
            const int x0 = static_cast<int>(x / s);
            const int x1 = std::max(x0, static_cast<int>((x + 1) / s) - 1);
            double acc = 0.0;
            long cnt = 0;
            for (int sy = y0; sy <= y1; ++sy) {
                for (int sx = x0; sx <= x1; ++sx) {
                    acc += buf.pixel(sx, sy, 0) * 0.2126 +
                           buf.pixel(sx, sy, 1) * 0.7152 +
                           buf.pixel(sx, sy, 2) * 0.0722;
                    ++cnt;
                }
            }
            out[static_cast<size_t>(y) * w + x] =
                cnt > 0 ? static_cast<float>(acc / cnt) : 0.0f;
        }
    }
    return out;
}

double HdrMerge::estimateEv(const ImageBuffer& ref, const ImageBuffer& other) {
    int rw = 0, rh = 0, ow = 0, oh = 0;
    const std::vector<float> rl = lumaSmall(ref, 64, rw, rh);
    const std::vector<float> ol = lumaSmall(other, 64, ow, oh);
    const size_t n = std::min(rl.size(), ol.size());
    double sum = 0.0;
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        const float a = rl[i], b = ol[i];
        // Ignore pure black / clipped pixels where the ratio is meaningless.
        if (a > 1e-4f && b > 1e-4f && a < 0.999f && b < 0.999f) {
            sum += std::log2(b / a);
            ++count;
        }
    }
    return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

HdrMerge::Result HdrMerge::merge(const std::vector<ImageBufferPtr>& frames,
                                 const std::vector<double>& evOffsets,
                                 bool deghost, int bandRows) {
    Result result;
    if (frames.size() < 2) {
        result.error = "HDR merge needs at least two frames";
        return result;
    }

    // Validate every frame. No full-resolution F32 copies are made up front:
    // the merge streams row-bands below, so only a strip of each frame is
    // resident in F32 at any time instead of all frames at full resolution.
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!frames[i] || frames[i]->empty()) {
            result.error = "empty input frame";
            return result;
        }
    }
    const int w = frames[0]->width();
    const int h = frames[0]->height();
    for (size_t i = 0; i < frames.size(); ++i) {
        if (frames[i]->width() != w || frames[i]->height() != h) {
            result.error = "frames must share the same dimensions";
            return result;
        }
    }

    // Exposure offsets: use the caller's when valid, else estimate. The
    // estimate reads the downscaled luma of the original buffers; ImageBuffer
    // pixel access converts U16/U8 to the same floats the merge path below
    // would get from toF32, so the offsets are bit-identical to the old path
    // that read the converted full-frame F32 copies.
    result.evOffsets.assign(frames.size(), 0.0);
    if (evOffsets.size() == frames.size()) {
        for (size_t i = 0; i < frames.size(); ++i)
            result.evOffsets[i] = evOffsets[i];
    } else {
        for (size_t i = 1; i < frames.size(); ++i)
            result.evOffsets[i] = estimateEv(*frames[0], *frames[i]);
    }

    const size_t nf = frames.size();

    // Per-frame exposure gains that normalize each frame to the reference
    // (frame 0) exposure: a frame 1 stop brighter is divided by 2.
    std::vector<float> gains(nf);
    for (size_t i = 0; i < nf; ++i)
        gains[i] = static_cast<float>(std::pow(2.0, -result.evOffsets[i]));

    // Full-resolution F32 result, written row-band by row-band. Merging is
    // strictly per-pixel (no spatial dependency), so each band computes the
    // exact same values as a single pass over the whole image. The mutable
    // typed view makes the F32 invariant explicit instead of carrying it in a
    // raw reinterpret_cast.
    auto merged = ImageBuffer::create(w, h, PixelFormat::F32);
    const std::span<float> out = merged->asMutableFloatSpan();

    // Row-band height. The default (128) bounds the memory of the per-frame
    // band buffers; a test can pass a larger value to merge in one band and
    // verify the banded result is bit-identical.
    std::vector<float> lumas(nf);
    std::vector<float> rch(nf), gch(nf), bch(nf); // normalized channel values
    // Per-band read views, refilled for every band below. Declared here so the
    // inner pixel loop never re-allocates or re-validates a buffer.
    std::vector<std::span<const float>> bandSpans(nf);
    for (int y0 = 0; y0 < h; y0 += bandRows) {
        const int bandH = std::min(bandRows, h - y0);

        // Materialize just this band of each frame as F32. crop() copies the
        // raw bytes (preserving the source format); toF32() then converts a
        // U16/U8 strip to the exact floats the full-frame conversion produces.
        std::vector<ImageBufferPtr> bandF32(nf);
        for (size_t f = 0; f < nf; ++f) {
            auto band = frames[f]->crop(0, y0, w, bandH);
            bandF32[f] = (band->format() == PixelFormat::F32) ? band
                                                              : band->toF32();
        }
        // Typed read views. Every band buffer is F32 by construction (crop
        // preserves the source format; anything else was converted by toF32
        // above), so these views are non-empty and the per-pixel body needs no
        // format or bounds branch.
        for (size_t f = 0; f < nf; ++f)
            bandSpans[f] = bandF32[f]->asFloatSpan();

        const size_t bandPix = static_cast<size_t>(w) * bandH;
        const size_t outBase = static_cast<size_t>(y0) * w;
        for (size_t i = 0; i < bandPix; ++i) {
            // Normalize each frame's exposure and compute weights.
            double wsum = 0.0;
            double rAcc = 0.0, gAcc = 0.0, bAcc = 0.0;
            for (size_t f = 0; f < nf; ++f) {
                const float g = gains[f];
                const float* p = bandSpans[f].data() + i * 3;
                const float r = p[0] * g;
                const float gg = p[1] * g;
                const float b = p[2] * g;
                rch[f] = r;
                gch[f] = gg;
                bch[f] = b;
                const float L = luma(r, gg, b);
                lumas[f] = L;
                const float wt = wellExposed(L);
                wsum += wt;
                rAcc += wt * r;
                gAcc += wt * gg;
                bAcc += wt * b;
            }

            const float rMean = wsum > 1e-8 ? static_cast<float>(rAcc / wsum) : 0.0f;
            const float gMean = wsum > 1e-8 ? static_cast<float>(gAcc / wsum) : 0.0f;
            const float bMean = wsum > 1e-8 ? static_cast<float>(bAcc / wsum) : 0.0f;

            const size_t o = outBase + i;
            if (deghost) {
                // Robust median as the deghosted estimate; blend toward it where
                // the frames disagree (moving content / blown highlights).
                std::vector<float> rs(rch), gs(gch), bs(bch);
                const float rMed = medianOf(rs);
                const float gMed = medianOf(gs);
                const float bMed = medianOf(bs);

                double var = 0.0;
                for (size_t f = 0; f < nf; ++f) {
                    const float dr = rch[f] - rMed;
                    const float dg = gch[f] - gMed;
                    const float db = bch[f] - bMed;
                    var += dr * dr + dg * dg + db * db;
                }
                var /= nf;
                const float agreement = std::exp(-static_cast<float>(var) * 4.0f);
                out[o * 3 + 0] = rMed + (rMean - rMed) * agreement;
                out[o * 3 + 1] = gMed + (gMean - gMed) * agreement;
                out[o * 3 + 2] = bMed + (bMean - bMed) * agreement;
            } else {
                out[o * 3 + 0] = rMean;
                out[o * 3 + 1] = gMean;
                out[o * 3 + 2] = bMean;
            }
        }
    }

    result.linear = std::move(merged);
    return result;
}

} // namespace lrc
