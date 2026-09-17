// AiDenoiseProcessor implementation.
//
// Uses the AiInferenceEngine for real runs; a compiled-in bilateral
// filter provides identical semantics in builds without ONNX Runtime so
// denoise() always returns something useful.

#include "engine/AiDenoiseProcessor.h"
#include "engine/ColorManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace lrc {

// ---------------------------------------------------------------------------
// ctor / dtor
// ---------------------------------------------------------------------------
AiDenoiseProcessor::AiDenoiseProcessor(AiInferenceEngine* engine,
                                       const ColorManager* cm)
    : engine_(engine), cm_(cm) {}
AiDenoiseProcessor::~AiDenoiseProcessor() = default;

// ---------------------------------------------------------------------------
// Helpers: F32 buffer get/set (R,G,B floats interleaved)
// ---------------------------------------------------------------------------
static inline const float* pxPtr(const ImageBuffer& b, int x, int y) {
    return reinterpret_cast<const float*>(b.data().data()) +
           (static_cast<size_t>(y) * b.width() + x) * 3;
}
static inline float* pxPtr(ImageBuffer& b, int x, int y) {
    return reinterpret_cast<float*>(b.data().data()) +
           (static_cast<size_t>(y) * b.width() + x) * 3;
}

// ---------------------------------------------------------------------------
// fallabackBilateral — pure C++ bilateral filter (strength-aware)
// ---------------------------------------------------------------------------
ImageBufferPtr AiDenoiseProcessor::fallbackBilateral(const ImageBuffer& input,
                                                     float sigmaSpatial,
                                                     float sigmaRange,
                                                     int radius) {
    const int w = input.width(), h = input.height();
    auto out = ImageBuffer::create(w, h, PixelFormat::F32);
    if (input.format() != PixelFormat::F32) {
        // Convert via toF32 first (keeps this function generic)
        auto f32 = input.toF32();
        return fallbackBilateral(*f32, sigmaSpatial, sigmaRange, radius);
    }

    const float s2inv = -0.5f / (sigmaSpatial * sigmaSpatial);
    const float r2inv = -0.5f / (sigmaRange * sigmaRange);

    // Precompute spatial Gaussian LUT
    std::vector<float> spatLut((radius + 1) * (radius + 1));
    for (int dy = 0; dy <= radius; ++dy)
        for (int dx = 0; dx <= radius; ++dx) {
            const float d2 = float(dx*dx + dy*dy);
            spatLut[dy*(radius+1) + dx] = std::exp(d2 * s2inv);
        }

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float sumW = 0.0f;
            float sumR = 0.0f, sumG = 0.0f, sumB = 0.0f;
            const float* ctr = pxPtr(input, x, y);
            const float cr = ctr[0], cg = ctr[1], cb = ctr[2];
            for (int dy = -radius; dy <= radius; ++dy) {
                const int yy = y + dy;
                if (yy < 0 || yy >= h) continue;
                const int ady = std::abs(dy);
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int xx = x + dx;
                    if (xx < 0 || xx >= w) continue;
                    const int adx = std::abs(dx);
                    const float ws = spatLut[ady*(radius+1) + adx];
                    const float* p = pxPtr(input, xx, yy);
                    const float dr = p[0] - cr;
                    const float dg = p[1] - cg;
                    const float db = p[2] - cb;
                    const float d2 = dr*dr + dg*dg + db*db;
                    const float wr = std::exp(d2 * r2inv);
                    const float w = ws * wr;
                    sumW += w;
                    sumR += w * p[0];
                    sumG += w * p[1];
                    sumB += w * p[2];
                }
            }
            float* d = pxPtr(*out, x, y);
            if (sumW > 1e-6f) {
                d[0] = sumR / sumW;
                d[1] = sumG / sumW;
                d[2] = sumB / sumW;
            } else {
                d[0] = cr; d[1] = cg; d[2] = cb;
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// blend
// ---------------------------------------------------------------------------
ImageBufferPtr AiDenoiseProcessor::blend(const ImageBuffer& a,
                                         const ImageBuffer& b,
                                         float strength) {
    const int w = a.width(), h = a.height();
    if (b.width() != w || b.height() != h) return std::make_shared<ImageBuffer>(a);
    const float s = std::clamp(strength, 0.0f, 1.0f);
    const float t = 1.0f - s;
    auto out = ImageBuffer::create(w, h, PixelFormat::F32);
    const float* af = reinterpret_cast<const float*>(a.data().data());
    const float* bf = reinterpret_cast<const float*>(b.data().data());
    float* of = reinterpret_cast<float*>(out->data().data());
    const size_t n = static_cast<size_t>(w) * h * 3;
    for (size_t i = 0; i < n; ++i) of[i] = af[i] * t + bf[i] * s;
    return out;
}

// ---------------------------------------------------------------------------
// runTiledInference — splits an F32 ProPhoto buffer into (possibly
// overlapping) tiles, runs each tile through the engine, and feather-blends
// the overlaps.
// ---------------------------------------------------------------------------
ImageBufferPtr AiDenoiseProcessor::runTiledInference(AiInferenceEngine* engine,
                                                     const ImageBuffer& input,
                                                     const std::string& modelId,
                                                     int tileSize, int overlap) {
    const int w = input.width(), h = input.height();
    if (!engine || w <= 0 || h <= 0) return input.toF32();
    if (tileSize < 32) tileSize = 32;
    if (overlap < 0)   overlap = 0;
    if (overlap >= tileSize / 2) overlap = tileSize / 2 - 1;

    auto out = ImageBuffer::create(w, h, PixelFormat::F32);
    std::vector<float> weight(static_cast<size_t>(w) * h, 0.0f);
    float* outPx = reinterpret_cast<float*>(out->data().data());
    std::fill_n(outPx, static_cast<size_t>(w) * h * 3, 0.0f);

    // Precompute a 1D feathering LUT of length tileSize: the blend weight
    // for a pixel at position i within a tile.  Inside the "core" region
    // (overlap..tileSize-overlap) it's 1.0; edges fade linearly to 0 at the
    // very boundary.  Per-axis product gives the 2D weight.
    std::vector<float> feather(static_cast<size_t>(tileSize));
    for (int i = 0; i < tileSize; ++i) {
        float v = 1.0f;
        if (i < overlap)       v = (i + 1.0f) / (overlap + 1.0f);
        int tailDist = tileSize - 1 - i;
        if (tailDist < overlap) v = std::min(v, (tailDist + 1.0f) / (overlap + 1.0f));
        feather[i] = v;
    }

    ModelShape outShape;
    outShape.dims = {1, 3, -1, -1};
    outShape.layout = "NCHW";

    const int step = tileSize - overlap;
    for (int ty0 = 0; ty0 < h; ty0 += step) {
        for (int tx0 = 0; tx0 < w; tx0 += step) {
            const int tx1 = std::min(tx0 + tileSize, w);
            const int ty1 = std::min(ty0 + tileSize, h);
            const int tw = tx1 - tx0, th = ty1 - ty0;

            // Pad-reflect the tile to exactly tileSize x tileSize so the
            // network sees the expected shape (many networks require it).
            auto tile = ImageBuffer::create(tileSize, tileSize, PixelFormat::F32);
            float* tp = reinterpret_cast<float*>(tile->data().data());
            for (int y = 0; y < tileSize; ++y) {
                int ys = ty0 + y;
                if (ys >= h) ys = h - 1 - (ys - (h - 1));  // reflect
                if (ys < 0) ys = -ys;
                for (int x = 0; x < tileSize; ++x) {
                    int xs = tx0 + x;
                    if (xs >= w) xs = w - 1 - (xs - (w - 1));
                    if (xs < 0) xs = -xs;
                    const float* s = pxPtr(input, xs, ys);
                    const size_t di = (static_cast<size_t>(y) * tileSize + x) * 3;
                    tp[di]     = s[0];
                    tp[di + 1] = s[1];
                    tp[di + 2] = s[2];
                }
            }

            ImageBufferPtr denoisedTile;
            if (engine->hasModel(modelId)) {
                denoisedTile = engine->runImage(modelId, *tile, outShape);
                // runImage returns F32; if it failed we fall through.
            }
            if (!denoisedTile) {
                // Engine returned null -> use the raw tile as its own
                // denoised version (avoids black holes in the output even
                // when the model is missing mid-pipeline).
                denoisedTile = tile;
            }
            // Ensure the tile is exactly tileSize^2 (some networks may crop)
            if (denoisedTile->width() != tileSize || denoisedTile->height() != tileSize) {
                auto r = denoisedTile->resize(tileSize, tileSize);
                denoisedTile = r->toF32();
            }

            // Feather-blend into the accumulator
            const float* dp = reinterpret_cast<const float*>(denoisedTile->data().data());
            for (int y = 0; y < th; ++y) {
                const int fy = y;   // feather index within tile
                for (int x = 0; x < tw; ++x) {
                    const int fx = x;
                    const float fw = feather[fx] * feather[fy];
                    const int ox = tx0 + x, oy = ty0 + y;
                    const size_t di = (static_cast<size_t>(oy) * w + ox) * 3;
                    const size_t si = (static_cast<size_t>(y) * tileSize + x) * 3;
                    outPx[di    ] += fw * dp[si    ];
                    outPx[di + 1] += fw * dp[si + 1];
                    outPx[di + 2] += fw * dp[si + 2];
                    weight[static_cast<size_t>(oy) * w + ox] += fw;
                }
            }
        }
    }

    // Normalise each pixel by its accumulated weight (should be ~1.0 in the
    // middle, less at the absolute image edges).
    for (size_t i = 0, j = 0; i < weight.size(); ++i, j += 3) {
        const float wght = weight[i];
        if (wght > 1e-4f) {
            const float inv = 1.0f / wght;
            outPx[j    ] *= inv;
            outPx[j + 1] *= inv;
            outPx[j + 2] *= inv;
        } else {
            // Copy from input (shouldn't happen but keeps the image valid)
            const float* s = pxPtr(input, static_cast<int>(i % w),
                                   static_cast<int>(i / w));
            outPx[j] = s[0]; outPx[j+1] = s[1]; outPx[j+2] = s[2];
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// denoise() — main entry point.  Pipeline-aware: picks the right model and
// converts between working / sRGB spaces when needed.
// ---------------------------------------------------------------------------
ImageBufferPtr AiDenoiseProcessor::denoise(const ImageBuffer& input,
                                           const std::string& modelId,
                                           const AiDenoiseOptions& opts) {
    const int w = input.width(), h = input.height();
    if (w <= 0 || h <= 0) return std::make_shared<ImageBuffer>(input);
    if (opts.strength <= 0.0f) return input.toF32();

    ImageBufferPtr working;
    if (input.format() != PixelFormat::F32) working = input.toF32();
    else working = std::make_shared<ImageBuffer>(input);

    // Colour-space round-trip for models trained on sRGB: the spec allows
    // "ProPhoto -> sRGB -> denoise -> ProPhoto".  We only do the round-trip
    // when we have a real ColorManager available.
    std::shared_ptr<ImageBuffer> srgbView;
    if (cm_ && !modelId.empty() &&
        (modelId.find("srgb") != std::string::npos ||
         modelId.find("nafnet") != std::string::npos ||
         modelId.find("dncnn") != std::string::npos ||
         modelId.find("ffdnet") != std::string::npos)) {
        // Convert linear ProPhoto F32 into F32 sRGB (gamma encoded, [0,1]).
        srgbView = ImageBuffer::create(w, h, PixelFormat::F32);
        const float* src = reinterpret_cast<const float*>(working->data().data());
        float* dst = reinterpret_cast<float*>(srgbView->data().data());
        cm_->linearProPhotoToSRGB(src, dst, static_cast<size_t>(w) * h);
        working = srgbView;
    }

    ImageBufferPtr denoised;
    const bool haveModel = engine_ && !modelId.empty() && engine_->hasModel(modelId);
    if (haveModel) {
        denoised = runTiledInference(engine_, *working, modelId,
                                     opts.tileSize, opts.overlap);
    }
    if (!denoised) {
        // Fallback bilateral filter (always available).  Roughly match the
        // user's strength: s=0.25 -> light NR, s=1.0 -> strong.
        const float s = std::clamp(opts.strength, 0.0f, 1.0f);
        const float sp   = 1.0f + s * 2.0f;          // 1..3 px
        const float rng  = 0.05f + s * 0.07f;        // 0.05..0.12
        const int   rad  = 2 + static_cast<int>(s * 2); // 2..4
        denoised = fallbackBilateral(*working, sp, rng, rad);
    }

    // (Optional) luminance-only: convert the denoised result + original to
    // YUV, replace only Y with the denoised Y, convert back to RGB.  Keeps
    // chroma noise untouched when the user only wants luma NR.  We do this
    // on the working-space buffer.
    if (opts.luminanceOnly) {
        const size_t n = static_cast<size_t>(w) * h;
        const float* origPx = reinterpret_cast<const float*>(working->data().data());
        const float* dnPx   = reinterpret_cast<const float*>(denoised->data().data());
        auto result = ImageBuffer::create(w, h, PixelFormat::F32);
        float* outPx = reinterpret_cast<float*>(result->data().data());
        for (size_t i = 0; i < n; ++i) {
            const size_t j = i * 3;
            const float r = origPx[j], g = origPx[j+1], b = origPx[j+2];
            const float dr = dnPx[j], dg = dnPx[j+1], db = dnPx[j+2];
            // Rec.709 luminance for the denoised result
            const float yDN = 0.2126f*dr + 0.7152f*dg + 0.0722f*db;
            const float y   = 0.2126f*r  + 0.7152f*g  + 0.0722f*b;
            const float dy = yDN - y;
            outPx[j  ] = std::clamp(r + dy, 0.0f, 1.0f);
            outPx[j+1] = std::clamp(g + dy, 0.0f, 1.0f);
            outPx[j+2] = std::clamp(b + dy, 0.0f, 1.0f);
        }
        denoised = result;
    }

    // sRGB -> ProPhoto reverse round-trip if we converted earlier
    if (srgbView && cm_ && denoised) {
        // Convert sRGB F32 (gamma) [0,1] back into ProPhoto linear.
        // lcms2's srgbToLinearProPhoto only takes U8; since we have the
        // ColorManager API we reuse the per-pixel function + matrix approach.
        auto linearOut = ImageBuffer::create(w, h, PixelFormat::F32);
        const float* srgbPx = reinterpret_cast<const float*>(denoised->data().data());
        float* linPx = reinterpret_cast<float*>(linearOut->data().data());
        // Go via 8-bit sRGB -> linear ProPhoto conversion (ColorManager's
        // public path).  sRGB F32 -> U8 clamp+round is acceptable quality
        // for a denoise preview (details have been softened anyway).
        auto u8Buf = ImageBuffer::create(w, h, PixelFormat::U8);
        uint8_t* u8 = u8Buf->data().data();
        for (size_t i = 0; i < static_cast<size_t>(w)*h*3; ++i) {
            const float v = std::clamp(srgbPx[i], 0.0f, 1.0f);
            u8[i] = static_cast<uint8_t>(v * 255.0f + 0.5f);
        }
        auto proPhoto = cm_->srgbToLinearProPhoto(*u8Buf);
        if (proPhoto && proPhoto->format() == PixelFormat::F32) {
            return blend(input, *proPhoto, opts.strength);
        }
        // Fallthrough: couldn't convert back -> blend in sRGB space after all
    }

    return blend(input, *denoised, opts.strength);
}

} // namespace lrc
