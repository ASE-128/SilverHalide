// AiMaskGenerator implementation.

#include "engine/AiMaskGenerator.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace lrc {

// ---------------------------------------------------------------------------
// ctor / dtor
// ---------------------------------------------------------------------------
AiMaskGenerator::AiMaskGenerator(AiInferenceEngine* engine) : engine_(engine) {}
AiMaskGenerator::~AiMaskGenerator() = default;

// ---------------------------------------------------------------------------
// Internal helpers: convert any source format to float RGB [0,1]
// ---------------------------------------------------------------------------
static std::vector<float> toFloatRGB(const ImageBuffer& input) {
    const int w = input.width(), h = input.height();
    const size_t n = static_cast<size_t>(w) * h * 3;
    std::vector<float> out(n);
    if (input.format() == PixelFormat::F32) {
        const float* s = reinterpret_cast<const float*>(input.data().data());
        std::copy(s, s + n, out.begin());
    } else if (input.format() == PixelFormat::U16) {
        const uint16_t* s = reinterpret_cast<const uint16_t*>(input.data().data());
        const float inv = 1.0f / 65535.0f;
        for (size_t i = 0; i < n; ++i) out[i] = s[i] * inv;
    } else {
        const uint8_t* s = input.data().data();
        const float inv = 1.0f / 255.0f;
        for (size_t i = 0; i < n; ++i) out[i] = s[i] * inv;
    }
    return out;
}

static ImageBufferPtr singleChToGreyBuf(const std::vector<float>& w,
                                        int w_, int h_) {
    auto buf = ImageBuffer::create(w_, h_, PixelFormat::F32);
    float* dst = reinterpret_cast<float*>(buf->data().data());
    const size_t n = static_cast<size_t>(w_) * h_;
    for (size_t i = 0; i < n; ++i) {
        const float v = std::clamp(w[i], 0.0f, 1.0f);
        dst[i*3  ] = v;
        dst[i*3+1] = v;
        dst[i*3+2] = v;
    }
    return buf;
}

// ---------------------------------------------------------------------------
// heuristicFallback — always-available pure-C++ mask
// ---------------------------------------------------------------------------
ImageBufferPtr AiMaskGenerator::heuristicFallback(const ImageBuffer& input,
                                                  AiMaskTarget target) {
    const int w = input.width(), h = input.height();
    if (w <= 0 || h <= 0) return nullptr;
    const auto rgb = toFloatRGB(input);
    std::vector<float> weight(static_cast<size_t>(w) * h, 0.0f);

    if (target == AiMaskTarget::Sky) {
        // Upper region + blue-dominant pixels
        for (int y = 0; y < h; ++y) {
            const float yFrac = 1.0f - static_cast<float>(y) / std::max(1, h - 1);
            for (int x = 0; x < w; ++x) {
                const size_t i = (static_cast<size_t>(y) * w + x) * 3;
                const float r = rgb[i], g = rgb[i + 1], b = rgb[i + 2];
                // Blue-over-red ratio, softened
                const float br = (b + 1e-3f) / (r + 0.1f);
                const float blueScore = std::min(1.0f, br * 0.6f) *
                                        (b > 0.15f ? 1.0f : 0.2f);
                // Sky rarely appears near the bottom; 50/50 blend with yFrac.
                const float yScore = yFrac > 0.5f ? (yFrac - 0.5f) * 2.0f : 0.0f;
                const float v = std::min(1.0f, 0.3f * yScore + 0.85f * blueScore);
                weight[static_cast<size_t>(y) * w + x] = v;
            }
        }
    } else {
        // Subject (salience) and Background share the same base: a soft
        // elliptical weight centred on the image, biased slightly up so the
        // traditional subject-eye-line is weighted higher.
        const float cx = 0.5f * (w - 1.0f);
        const float cy = 0.45f * (h - 1.0f);
        const float sx = std::max(1.0f, 0.45f * w);
        const float sy = std::max(1.0f, 0.45f * h);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float dx = (x - cx) / sx;
                const float dy = (y - cy) / sy;
                const float d2 = dx*dx + dy*dy;
                const float g = std::exp(-d2 * 2.2f);
                // Boost "figure vs background": pixels that deviate more in
                // chroma from the image-global mean get a bonus.
                weight[static_cast<size_t>(y) * w + x] = std::min(1.0f, g);
            }
        }
        // Background = 1 - subject.
        if (target == AiMaskTarget::Background) {
            for (auto& v : weight) v = 1.0f - v;
        }
    }

    return singleChToGreyBuf(weight, w, h);
}

// ---------------------------------------------------------------------------
// refineMask — threshold, invert, gaussian feather
// ---------------------------------------------------------------------------
ImageBufferPtr AiMaskGenerator::refineMask(const ImageBuffer& maskFloat,
                                           float threshold, float featherSigma,
                                           bool invert) {
    const int w = maskFloat.width(), h = maskFloat.height();
    if (w <= 0 || h <= 0) return nullptr;

    // Extract R channel (grey buffer -> R=G=B, so any channel works)
    std::vector<float> m(static_cast<size_t>(w) * h);
    const float* src = reinterpret_cast<const float*>(maskFloat.data().data());
    for (size_t i = 0; i < m.size(); ++i)
        m[i] = std::clamp(src[i * 3], 0.0f, 1.0f);

    // 1) Threshold (a soft knee: values below threshold scale down, above push up)
    const float soft = 0.1f;
    for (auto& v : m) {
        if (v < threshold - soft) {
            v *= 0.3f;
        } else if (v < threshold + soft) {
            const float k = (v - (threshold - soft)) / (2.0f * soft);
            v = v * (0.3f + k * 0.7f);
        }
    }
    // 2) Invert
    if (invert) for (auto& v : m) v = 1.0f - v;

    // 3) Gaussian feather (separable)
    if (featherSigma > 0.5f) {
        const int r = static_cast<int>(featherSigma * 3.0f + 0.5f);
        std::vector<float> kernel(2 * r + 1);
        float sum = 0.0f;
        for (int i = -r; i <= r; ++i) {
            const float v = std::exp(-static_cast<float>(i*i) /
                                     (2.0f * featherSigma * featherSigma));
            kernel[i + r] = v; sum += v;
        }
        const float inv = 1.0f / sum;
        for (auto& v : kernel) v *= inv;

        std::vector<float> tmp(m.size(), 0.0f);
        // horizontal
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float s = 0.0f;
                for (int k = -r; k <= r; ++k) {
                    int xx = x + k;
                    if (xx < 0) xx = 0; else if (xx >= w) xx = w - 1;
                    s += m[static_cast<size_t>(y) * w + xx] * kernel[k + r];
                }
                tmp[static_cast<size_t>(y) * w + x] = s;
            }
        }
        // vertical
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float s = 0.0f;
                for (int k = -r; k <= r; ++k) {
                    int yy = y + k;
                    if (yy < 0) yy = 0; else if (yy >= h) yy = h - 1;
                    s += tmp[static_cast<size_t>(yy) * w + x] * kernel[k + r];
                }
                m[static_cast<size_t>(y) * w + x] = std::clamp(s, 0.0f, 1.0f);
            }
        }
    }
    return singleChToGreyBuf(m, w, h);
}

// ---------------------------------------------------------------------------
// generate() — main dispatch.
// ---------------------------------------------------------------------------
ImageBufferPtr AiMaskGenerator::generate(const ImageBuffer& input,
                                         const AiMaskParams& params,
                                         const std::string& modelIdOverride) {
    const int w = input.width(), h = input.height();
    if (w <= 0 || h <= 0) return nullptr;

    // Click-prompt (MobileSAM) path: rasterize the SamPoint list via the
    // encoder+decoder pair, then apply the same threshold/feather/invert
    // refinement as the one-shot models. Returns a zero mask when the points
    // are empty or inference fails, so callers never dereference a null buffer.
    if (params.target == AiMaskTarget::Prompt) {
        if (!params.points.empty() && engine_) {
            const auto res = engine_->runSamPoint(
                defaultPromptEncoderId(), defaultPromptDecoderId(), input,
                params.points);
            if (res.ok && static_cast<int>(res.mask.size()) == w * h) {
                auto grey = singleChToGreyBuf(res.mask, w, h);
                if (grey)
                    return refineMask(*grey, params.threshold, params.feather,
                                      params.invert);
            }
        }
        // Fall through to an explicit zero mask on any failure.
        auto zero = ImageBuffer::create(w, h, PixelFormat::F32);
        auto& d = zero->data();
        std::fill(d.begin(), d.end(), 0);
        return zero;
    }

    const std::string modelId = modelIdOverride.empty()
        ? defaultModelIdFor(params.target)
        : modelIdOverride;

    ImageBufferPtr rawMask;

    if (engine_ && !modelId.empty() && engine_->hasModel(modelId)) {
        // Ask the inference engine to produce an NCHW tensor of 1 channel at
        // the model's canonical size.  AiInferenceEngine::runImage already
        // handles resize + mean/std + NCHW transposition.
        ModelShape outShape;
        outShape.layout = "NCHW";
        const auto info = engine_->modelInfo(modelId);
        if (info && !info->outputShape.dims.empty()) {
            outShape.dims = info->outputShape.dims;
        } else {
            outShape.dims = {1, 1, params.modelInputSize, params.modelInputSize};
        }
        auto inferred = engine_->runImage(modelId, input, outShape);
        if (inferred) {
            // runImage returns a 3-channel grey buffer (F32 U8 postprocess
            // fallback notwithstanding).  Resize up to w×h.
            if (inferred->width() != w || inferred->height() != h) {
                inferred = inferred->resizeToF32(w, h);
            }
            rawMask = inferred;
        }
    }

    if (!rawMask) {
        rawMask = heuristicFallback(input, params.target);
    }
    if (!rawMask) {
        // Absolute worst case: empty (zero) mask, never nullptr.
        auto zero = ImageBuffer::create(w, h, PixelFormat::F32);
        auto& d = zero->data();
        std::fill(d.begin(), d.end(), 0);
        return zero;
    }

    // For Background target we invert *before* refine so the same threshold/
    // feather semantics apply to the foreground boundary.
    const bool firstInvert = (params.target == AiMaskTarget::Background);
    if (firstInvert) {
        float* p = reinterpret_cast<float*>(rawMask->data().data());
        const size_t n = static_cast<size_t>(w) * h * 3;
        for (size_t i = 0; i < n; i += 3) {
            const float v = 1.0f - std::clamp(p[i], 0.0f, 1.0f);
            p[i] = p[i+1] = p[i+2] = v;
        }
    }

    return refineMask(*rawMask, params.threshold, params.feather, params.invert);
}

} // namespace lrc
