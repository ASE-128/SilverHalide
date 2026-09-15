#include "engine/HdrMerge.h"

#include <algorithm>
#include <cmath>
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

    // Area-average downscale of the luminance.
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

    const auto px = [&](const ImageBufferPtr& b, size_t i, int c) {
        const auto* d = reinterpret_cast<const float*>(b->data().data());
        return d[i * 3 + c];
    };

    // Full-resolution F32 result, written row-band by row-band. Merging is
    // strictly per-pixel (no spatial dependency), so each band computes the
    // exact same values as a single pass over the whole image.
    auto merged = ImageBuffer::create(w, h, PixelFormat::F32);
    auto* out = reinterpret_cast<float*>(merged->data().data());

    // Row-band height. The default (128) bounds the memory of the per-frame
    // band buffers; a test can pass a larger value to merge in one band and
    // verify the banded result is bit-identical.
    std::vector<float> lumas(nf);
    std::vector<float> rch(nf), gch(nf), bch(nf); // normalized channel values
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

        const size_t bandPix = static_cast<size_t>(w) * bandH;
        const size_t outBase = static_cast<size_t>(y0) * w;
        for (size_t i = 0; i < bandPix; ++i) {
            // Normalize each frame's exposure and compute weights.
            double wsum = 0.0;
            double rAcc = 0.0, gAcc = 0.0, bAcc = 0.0;
            for (size_t f = 0; f < nf; ++f) {
                const float g = gains[f];
                const float r = px(bandF32[f], i, 0) * g;
                const float gg = px(bandF32[f], i, 1) * g;
                const float b = px(bandF32[f], i, 2) * g;
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
