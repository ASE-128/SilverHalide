#include "engine/PanoramaStitcher.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "utils/MathUtils.h"
#include "utils/Parallel.h"

namespace lrc {

namespace {

// ---- alignment constants --------------------------------------------------

// Minimum NCC score for an accepted translation estimate. Below this the pair
// is treated as having no reliable overlap and stitchPanorama() throws.
constexpr float kMinNccScore = 0.25f;

// NCC scores closer than this are treated as ties. The preferred candidate is
// then the one with the smaller vertical offset (then horizontal) -- a
// conservative bias that keeps a mostly-horizontal panorama from locking onto
// a spurious dy when the image has weak vertical variation.
constexpr float kScoreEpsilon = 0.0001f;

// Minimum overlap area (fraction of the smaller image) a candidate offset must
// have before its NCC score is considered meaningful.
constexpr float kMinOverlapFraction = 0.03f;

// Feather margin: the blend weight ramps from 0 at the source boundary to 1
// over this fraction of the image's smaller side.
constexpr float kFeatherFraction = 0.2f;

// Sensor width (mm) used to convert the focal length from millimetres to
// pixels: f_px = focalLengthMm * imageWidth / kSensorWidthMm.
constexpr float kSensorWidthMm = 36.0f;

// ---- small helpers --------------------------------------------------------

inline float lumaOf(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// Extracts Rec.709 luminance from an F32 RGB buffer (row-major, 1 channel).
std::vector<float> lumaImage(const ImageBuffer& buf) {
    const int w = buf.width();
    const int h = buf.height();
    std::vector<float> out(static_cast<size_t>(w) * h);
    const float* d = reinterpret_cast<const float*>(buf.data().data());
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = lumaOf(d[i * 3], d[i * 3 + 1], d[i * 3 + 2]);
    return out;
}

struct ShiftResult {
    float dx = 0.0f;
    float dy = 0.0f;
    float score = -1.0e9f;
    bool ok = false;
};

// Returns true if candidate (s, dx, dy) is a better match than the running
// best (bs, bdx, bdy). Scores within kScoreEpsilon are treated as ties; the
// preferred candidate is then the one with the smaller |dy| (then |dx|), which
// biases panorama alignment toward conservative translations. This keeps a
// mostly-horizontal pair from locking onto a spurious dy when the image's
// vertical variation is too weak for NCC to disambiguate it.
inline bool betterMatch(float s, float dx, float dy, float bs, float bdx,
                        float bdy) {
    if (s > bs + kScoreEpsilon)
        return true;
    if (s < bs - kScoreEpsilon)
        return false;
    if (std::fabs(dy) != std::fabs(bdy))
        return std::fabs(dy) < std::fabs(bdy);
    return std::fabs(dx) < std::fabs(bdx);
}

// Estimates the translation that best aligns B onto A:
//   B(x, y) ~= A(x + dx, y + dy)
// A and B are luma images of size (aw, ah) and (bw, bh) respectively. The
// search is a coarse grid followed by two refinement passes; NCC is computed
// directly in the spatial domain (no FFT), so only translations are estimated.
ShiftResult estimateShift(const std::vector<float>& A, int aw, int ah,
                          const std::vector<float>& B, int bw, int bh) {
    const int maxDim = std::max({aw, ah, bw, bh});
    const int dxSearch = static_cast<int>(std::ceil(0.95f * maxDim));
    const int dySearch = static_cast<int>(std::ceil(0.95f * maxDim));
    const int s1 = std::max(1, maxDim / 64);   // coarse step (fine enough to land near typical shifts)
    const int s2 = std::max(1, s1 / 4);        // medium step
    const long minOverlap =
        static_cast<long>(kMinOverlapFraction *
                          std::min(static_cast<long>(aw) * ah,
                                   static_cast<long>(bw) * bh));

    // NCC of A and B at offset (dx, dy), computed over their overlap.
    auto scoreAt = [&](int dx, int dy) -> float {
        const int x0 = std::max(0, -dx);
        const int x1 = std::min(bw, aw - dx);
        const int y0 = std::max(0, -dy);
        const int y1 = std::min(bh, ah - dy);
        const int ow = x1 - x0;
        const int oh = y1 - y0;
        if (ow < 1 || oh < 1)
            return -1.0e9f;
        const long n = static_cast<long>(ow) * oh;
        if (n < minOverlap)
            return -1.0e9f;
        double sumA = 0.0, sumB = 0.0, sumAA = 0.0, sumBB = 0.0, sumAB = 0.0;
        for (int y = y0; y < y1; ++y) {
            const float* aRow = A.data() + static_cast<size_t>(y + dy) * aw;
            const float* bRow = B.data() + static_cast<size_t>(y) * bw;
            for (int x = x0; x < x1; ++x) {
                const float a = aRow[x + dx];
                const float b = bRow[x];
                sumA += a;
                sumB += b;
                sumAA += static_cast<double>(a) * a;
                sumBB += static_cast<double>(b) * b;
                sumAB += static_cast<double>(a) * b;
            }
        }
        const double meanA = sumA / n;
        const double meanB = sumB / n;
        const double varA = sumAA - n * meanA * meanA;
        const double varB = sumBB - n * meanB * meanB;
        // Featureless overlap: NCC is undefined when either image has (almost)
        // no variation. The relative threshold keeps the check scale-invariant
        // and robust against the tiny positive variance that floating-point
        // rounding can produce for a perfectly flat input.
        if (varA <= 0.0 || varB <= 0.0 ||
            varA < 1.0e-9 * sumAA || varB < 1.0e-9 * sumBB)
            return -1.0e9f;
        const double cov = sumAB - n * meanA * meanB;
        return static_cast<float>(cov / std::sqrt(varA * varB));
    };

    float bestScore = -1.0e9f;
    float bestDx = 0.0f;
    float bestDy = 0.0f;
    bool found = false;

    // Coarse pass. The candidate grid is flattened and scored in parallel;
    // each candidate writes its own slot, so the chunked parallelFor is race
    // free. With s1 = maxDim/64 the grid is at most ~15k candidates at
    // alignMaxDim=512, which is cheap on the downscaled luma image.
    std::vector<int> cdx, cdy;
    cdx.reserve(static_cast<size_t>(2 * dxSearch / s1 + 3));
    cdy.reserve(static_cast<size_t>(2 * dySearch / s1 + 3));
    for (int dy = -dySearch; dy <= dySearch; dy += s1)
        for (int dx = -dxSearch; dx <= dxSearch; dx += s1) {
            cdx.push_back(dx);
            cdy.push_back(dy);
        }
    const size_t nCoarse = cdx.size();
    if (nCoarse == 0)
        return {};
    std::vector<float> coarseScores(nCoarse, -1.0e9f);
    lrc::parallel::parallelFor(nCoarse, lrc::parallel::defaultGrain(nCoarse),
                               [&](size_t b, size_t e) {
        for (size_t i = b; i < e; ++i)
            coarseScores[i] = scoreAt(cdx[i], cdy[i]);
    });
    for (size_t i = 0; i < nCoarse; ++i) {
        if (!found || betterMatch(coarseScores[i],
                                  static_cast<float>(cdx[i]),
                                  static_cast<float>(cdy[i]), bestScore,
                                  bestDx, bestDy)) {
            bestScore = coarseScores[i];
            bestDx = static_cast<float>(cdx[i]);
            bestDy = static_cast<float>(cdy[i]);
            found = true;
        }
    }
    if (!found)
        return {};

    // Medium refinement around the coarse best.
    {
        const int bx = static_cast<int>(bestDx);
        const int by = static_cast<int>(bestDy);
        for (int dy = by - 2 * s1; dy <= by + 2 * s1; dy += s2) {
            for (int dx = bx - 2 * s1; dx <= bx + 2 * s1; dx += s2) {
                const float s = scoreAt(dx, dy);
                if (betterMatch(s, static_cast<float>(dx),
                                static_cast<float>(dy), bestScore, bestDx,
                                bestDy)) {
                    bestScore = s;
                    bestDx = static_cast<float>(dx);
                    bestDy = static_cast<float>(dy);
                }
            }
        }
    }

    // Fine pass (step 1) around the medium best.
    {
        const int bx = static_cast<int>(bestDx);
        const int by = static_cast<int>(bestDy);
        for (int dy = by - 2 * s2; dy <= by + 2 * s2; ++dy) {
            for (int dx = bx - 2 * s2; dx <= bx + 2 * s2; ++dx) {
                const float s = scoreAt(dx, dy);
                if (betterMatch(s, static_cast<float>(dx),
                                static_cast<float>(dy), bestScore, bestDx,
                                bestDy)) {
                    bestScore = s;
                    bestDx = static_cast<float>(dx);
                    bestDy = static_cast<float>(dy);
                }
            }
        }
    }

    ShiftResult r;
    r.dx = bestDx;
    r.dy = bestDy;
    r.score = bestScore;
    r.ok = bestScore >= kMinNccScore;
    return r;
}

// Projected geometry of a single input image on the panorama canvas.
struct ProjectedImage {
    float cx = 0.0f;        // image centre (px)
    float cy = 0.0f;
    float f = 0.0f;         // focal length in px (0 in pure-translation mode)
    float projMinX = 0.0f;  // projected x extent in image-local space
    float projMaxX = 0.0f;
    float projMinY = 0.0f;
    float projMaxY = 0.0f;
    int canvasXMin = 0;     // extent in canvas coordinates (inclusive)
    int canvasXMax = 0;
    int canvasYMin = 0;
    int canvasYMax = 0;
    float feather = 1.0f;
    // Inverse-projection lookup tables, indexed by k = canvasX - canvasXMin:
    //   invSx[k] = raw source x for that canvas column,
    //   invS[k]  = sqrt((sx - cx)^2 + f^2) for that canvas column.
    std::vector<float> invSx;
    std::vector<float> invS;
};

// Builds the projected geometry for one image whose top-left sits at (posX,
// posY) on the canvas. In cylindrical mode the focal length is estimated from
// the image width: f_px = focalLengthMm * width / 36 (35mm full-frame sensor).
ProjectedImage buildProjected(const ImageBuffer& img, float posX, float posY,
                              bool cylindrical, float focalLengthMm) {
    ProjectedImage p;
    const int w = img.width();
    const int h = img.height();
    p.cx = w * 0.5f;
    p.cy = h * 0.5f;
    p.feather = std::max(1.0f, std::min(w, h) * kFeatherFraction);

    if (cylindrical) {
        p.f = std::max(1.0f, focalLengthMm * static_cast<float>(w) /
                                 kSensorWidthMm);
        // x' = f * atan((x - cx) / f) + cx is monotonic in x, so the x extent
        // is reached at the image edges.
        p.projMinX = p.cx - p.f * std::atan(p.cx / p.f);            // x = 0
        p.projMaxX = p.cx + p.f * std::atan((w - 1 - p.cx) / p.f);  // x = w-1
        // y' = f * (y - cy) / sqrt((x - cx)^2 + f^2) + cy stays inside
        // [0, h-1] for every (x, y) because sqrt(...) >= f; the full height is
        // reached at the centre column, so the y extent is the image height.
        p.projMinY = 0.0f;
        p.projMaxY = h - 1.0f;
    } else {
        p.f = 0.0f;
        p.projMinX = 0.0f;
        p.projMaxX = w - 1.0f;
        p.projMinY = 0.0f;
        p.projMaxY = h - 1.0f;
    }

    p.canvasXMin = static_cast<int>(std::floor(posX + p.projMinX));
    p.canvasXMax = static_cast<int>(std::ceil(posX + p.projMaxX));
    p.canvasYMin = static_cast<int>(std::floor(posY + p.projMinY));
    p.canvasYMax = static_cast<int>(std::ceil(posY + p.projMaxY));

    // Precompute the inverse projection for every canvas column the image
    // touches so the per-pixel composite loop only does a table lookup.
    const int cols = p.canvasXMax - p.canvasXMin + 1;
    p.invSx.resize(static_cast<size_t>(cols));
    p.invS.resize(static_cast<size_t>(cols));
    for (int col = 0; col < cols; ++col) {
        const float canvasX = static_cast<float>(p.canvasXMin + col);
        const float relX = canvasX - posX;
        if (cylindrical) {
            const float sx = p.cx + p.f * std::tan((relX - p.cx) / p.f);
            const float dx = sx - p.cx;
            p.invSx[static_cast<size_t>(col)] = sx;
            p.invS[static_cast<size_t>(col)] = std::sqrt(dx * dx + p.f * p.f);
        } else {
            p.invSx[static_cast<size_t>(col)] = relX;
            p.invS[static_cast<size_t>(col)] = 0.0f;
        }
    }
    return p;
}

} // namespace

std::shared_ptr<ImageBuffer> stitchPanorama(
    const std::vector<ImageBufferPtr>& inputs, const PanoramaOptions& opts) {
    // Empty input vector -> nullptr.
    if (inputs.empty())
        return nullptr;

    // Validate members.
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (!inputs[i] || inputs[i]->empty())
            throw std::invalid_argument(
                "PanoramaStitcher: null or empty input image at index " +
                std::to_string(i));
    }

    // Single input: return an F32 copy with identical dimensions. (Same-size
    // copy, not a reference, so the caller keeps sole ownership.)
    if (inputs.size() == 1)
        return inputs[0]->toF32();

    const int alignMaxDim = std::max(1, opts.alignMaxDim);
    const int bandRows = std::max(1, opts.bandRows);
    const bool cylindrical = opts.cylindrical;
    const size_t n = inputs.size();

    // ---- alignment --------------------------------------------------------

    // Area-average downscale of every input to a common max edge.
    std::vector<ImageBufferPtr> small(n);
    std::vector<float> sxScale(n), syScale(n);
    for (size_t i = 0; i < n; ++i) {
        const int w = inputs[i]->width();
        const int h = inputs[i]->height();
        const int longest = std::max(w, h);
        const float scale = (longest > alignMaxDim)
                                ? static_cast<float>(alignMaxDim) / longest
                                : 1.0f;
        const int aw = std::max(1, static_cast<int>(std::lround(w * scale)));
        const int ah = std::max(1, static_cast<int>(std::lround(h * scale)));
        small[i] = inputs[i]->resizeToF32(aw, ah);
        sxScale[i] = static_cast<float>(w) / aw;
        syScale[i] = static_cast<float>(h) / ah;
    }

    // Accumulated canvas position of each image (image 0 anchors the canvas).
    std::vector<float> posX(n, 0.0f), posY(n, 0.0f);
    for (size_t i = 0; i + 1 < n; ++i) {
        const std::vector<float> lumaA = lumaImage(*small[i]);
        const std::vector<float> lumaB = lumaImage(*small[i + 1]);
        const ShiftResult sr = estimateShift(
            lumaA, small[i]->width(), small[i]->height(), lumaB,
            small[i + 1]->width(), small[i + 1]->height());
        if (!sr.ok) {
            throw std::runtime_error(
                "PanoramaStitcher: no reliable overlap between input images " +
                std::to_string(i) + " and " + std::to_string(i + 1) +
                " (best NCC score " + std::to_string(sr.score) + ")");
        }
        // Scale the downsampled offset back to full resolution using image i's
        // scale (the offset is expressed in image i's frame).
        posX[i + 1] =
            posX[i] + static_cast<float>(std::lround(sr.dx * sxScale[i]));
        posY[i + 1] =
            posY[i] + static_cast<float>(std::lround(sr.dy * syScale[i]));
    }

    // ---- projection and canvas --------------------------------------------

    std::vector<ProjectedImage> proj(n);
    int canvasX0 = std::numeric_limits<int>::max();
    int canvasY0 = std::numeric_limits<int>::max();
    int canvasX1 = std::numeric_limits<int>::min();
    int canvasY1 = std::numeric_limits<int>::min();
    for (size_t i = 0; i < n; ++i) {
        proj[i] = buildProjected(*inputs[i], posX[i], posY[i], cylindrical,
                                 opts.focalLengthMm);
        canvasX0 = std::min(canvasX0, proj[i].canvasXMin);
        canvasY0 = std::min(canvasY0, proj[i].canvasYMin);
        canvasX1 = std::max(canvasX1, proj[i].canvasXMax);
        canvasY1 = std::max(canvasY1, proj[i].canvasYMax);
    }
    const int canvasW = canvasX1 - canvasX0 + 1;
    const int canvasH = canvasY1 - canvasY0 + 1;
    if (canvasW <= 0 || canvasH <= 0)
        throw std::runtime_error("PanoramaStitcher: empty panorama canvas");

    // Sources are needed as F32 for direct float access during compositing.
    std::vector<ImageBufferPtr> srcF32(n);
    for (size_t i = 0; i < n; ++i)
        srcF32[i] = (inputs[i]->format() == PixelFormat::F32)
                        ? inputs[i]
                        : inputs[i]->toF32();

    auto out = ImageBuffer::create(canvasW, canvasH, PixelFormat::F32);
    float* outData = reinterpret_cast<float*>(out->data().data());

    // Per-band weight accumulator (the output RGB buffer itself is the
    // per-band float colour accumulator; the banding bounds the weight memory).
    std::vector<float> weightBuf(static_cast<size_t>(canvasW) * bandRows);

    // Content bounding box for autoCrop (in canvas coordinates).
    int contentX0 = canvasW, contentY0 = canvasH;
    int contentX1 = -1, contentY1 = -1;

    // ---- banded feather composite -----------------------------------------

    for (int bandY = 0; bandY < canvasH; bandY += bandRows) {
        const int bandH = std::min(bandRows, canvasH - bandY);
        std::fill(weightBuf.begin(),
                  weightBuf.begin() + static_cast<size_t>(canvasW) * bandH,
                  0.0f);

        // Accumulate every image into this band.
        for (size_t i = 0; i < n; ++i) {
            const ProjectedImage& p = proj[i];
            const int rowStart = std::max(bandY, p.canvasYMin - canvasY0);
            const int rowEnd =
                std::min(bandY + bandH, p.canvasYMax - canvasY0 + 1);
            if (rowStart >= rowEnd)
                continue;
            const int colStart = std::max(0, p.canvasXMin - canvasX0);
            const int colEnd = std::min(canvasW, p.canvasXMax - canvasX0 + 1);
            if (colStart >= colEnd)
                continue;

            const ImageBuffer& src = *srcF32[i];
            const int W = src.width();
            const int H = src.height();
            const float* srcData =
                reinterpret_cast<const float*>(src.data().data());
            const float cx = p.cx, cy = p.cy, f = p.f, feather = p.feather;
            const size_t rows = static_cast<size_t>(rowEnd - rowStart);

            // Grain 1 keeps the banded row work spread across the pool even for
            // small bands (a 64-row band would otherwise run inline and use a
            // single thread).
            lrc::parallel::parallelFor(rows, 1, [&](size_t by, size_t ey) {
                for (size_t r = by; r < ey; ++r) {
                    const int oy = rowStart + static_cast<int>(r);  // output row index
                    const int canvasY = canvasY0 + oy;              // canvas-absolute Y (projection)
                    const float relY = static_cast<float>(canvasY) - posY[i];
                    float* outPix =
                        outData + static_cast<size_t>(oy) * canvasW * 3;
                    float* wRow =
                        weightBuf.data() +
                        static_cast<size_t>(oy - bandY) * canvasW;
                    for (int col = colStart; col < colEnd; ++col) {
                        const int canvasX = canvasX0 + col;
                        const size_t k =
                            static_cast<size_t>(canvasX - p.canvasXMin);
                        const float sx = p.invSx[k];
                        float sy;
                        if (cylindrical) {
                            const float s = p.invS[k];
                            sy = cy + (relY - cy) * s / f;
                        } else {
                            sy = relY;
                        }
                        if (sx < 0.0f || sx >= static_cast<float>(W) ||
                            sy < 0.0f || sy >= static_cast<float>(H))
                            continue;   // outside this source image

                        // Bilinear sample.
                        const int x0 = static_cast<int>(sx);
                        const int y0 = static_cast<int>(sy);
                        const int x1 = std::min(x0 + 1, W - 1);
                        const int y1 = std::min(y0 + 1, H - 1);
                        const float tx = sx - x0;
                        const float ty = sy - y0;
                        const float wx0 = 1.0f - tx, wx1 = tx;
                        const float wy0 = 1.0f - ty, wy1 = ty;
                        const float* p00 =
                            srcData + (static_cast<size_t>(y0) * W + x0) * 3;
                        const float* p10 =
                            srcData + (static_cast<size_t>(y0) * W + x1) * 3;
                        const float* p01 =
                            srcData + (static_cast<size_t>(y1) * W + x0) * 3;
                        const float* p11 =
                            srcData + (static_cast<size_t>(y1) * W + x1) * 3;
                        const float r = (p00[0] * wx0 + p10[0] * wx1) * wy0 +
                                        (p01[0] * wx0 + p11[0] * wx1) * wy1;
                        const float g = (p00[1] * wx0 + p10[1] * wx1) * wy0 +
                                        (p01[1] * wx0 + p11[1] * wx1) * wy1;
                        const float b = (p00[2] * wx0 + p10[2] * wx1) * wy0 +
                                        (p01[2] * wx0 + p11[2] * wx1) * wy1;

                        // Feather weight: 0 at the source boundary, 1 in the
                        // interior. Kept above zero so the normalised blend
                        // never divides by zero.
                        const float d = std::min(
                            std::min(sx, static_cast<float>(W - 1) - sx),
                            std::min(sy, static_cast<float>(H - 1) - sy));
                        const float w = std::max(
                            static_cast<float>(lrc::smoothstep(
                                0.0, static_cast<double>(feather),
                                static_cast<double>(d))),
                            1.0e-4f);

                        const size_t off = static_cast<size_t>(col) * 3;
                        outPix[off] += w * r;
                        outPix[off + 1] += w * g;
                        outPix[off + 2] += w * b;
                        wRow[col] += w;
                    }
                }
            });
        }

        // Normalise the band and track the content bounding box.
        std::vector<int> rowMinX(static_cast<size_t>(bandH), canvasW);
        std::vector<int> rowMaxX(static_cast<size_t>(bandH), -1);
        // Grain 1 (same reasoning as the accumulation pass above).
        lrc::parallel::parallelFor(static_cast<size_t>(bandH), 1,
                                   [&](size_t by, size_t ey) {
            for (size_t r = by; r < ey; ++r) {
                float* outPix =
                    outData + static_cast<size_t>(bandY + r) * canvasW * 3;
                const float* wRow =
                    weightBuf.data() + static_cast<size_t>(r) * canvasW;
                int lx = canvasW, rx = -1;
                for (int c = 0; c < canvasW; ++c) {
                    const float w = wRow[c];
                    if (w > 1.0e-6f) {
                        const float inv = 1.0f / w;
                        outPix[c * 3] *= inv;
                        outPix[c * 3 + 1] *= inv;
                        outPix[c * 3 + 2] *= inv;
                        lx = std::min(lx, c);
                        rx = std::max(rx, c);
                    } else {
                        outPix[c * 3] = 0.0f;
                        outPix[c * 3 + 1] = 0.0f;
                        outPix[c * 3 + 2] = 0.0f;
                    }
                }
                rowMinX[r] = lx;
                rowMaxX[r] = rx;
            }
        });
        for (size_t r = 0; r < static_cast<size_t>(bandH); ++r) {
            if (rowMaxX[r] >= rowMinX[r]) {
                const int y = bandY + static_cast<int>(r);
                contentX0 = std::min(contentX0, rowMinX[r]);
                contentX1 = std::max(contentX1, rowMaxX[r]);
                contentY0 = std::min(contentY0, y);
                contentY1 = std::max(contentY1, y);
            }
        }
    }

    if (!opts.autoCrop)
        return out;

    // Crop to the content bounding box. This trims the empty 1-2 px margin left
    // by the floor/ceil rounding of the projected extents (and, for cylindrical
    // mode, the corner triangles where no image reaches).
    if (contentX0 >= contentX1 || contentY0 >= contentY1)
        return out;
    return out->crop(contentX0, contentY0, contentX1 - contentX0 + 1,
                     contentY1 - contentY0 + 1);
}

} // namespace lrc
