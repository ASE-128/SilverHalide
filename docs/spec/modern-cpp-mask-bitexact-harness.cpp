
// Mask bit-exactness oracle: pins the geometric weight rasterisation, the
// sub-mask combination, the cache hashes and the masked local-adjustment
// render path. Used to verify that refactoring Mask into a discriminated
// union (std::variant) does not change any observable output.
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <vector>
#include <memory>

#include "engine/Adjustments.h"
#include "engine/ColorManager.h"
#include "engine/ImageBuffer.h"
#include "engine/Pipeline.h"
#include "utils/Json.h"
#include "utils/ProfileIO.h"

using namespace lrc;

static uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
static uint64_t hashBuf(const ImageBuffer& b) { return fnv(b.data().data(), b.data().size()); }
static void emit(const char* name, uint64_t h) { std::printf("%s=%016llx\n", name, (unsigned long long)h); }

static ImageBufferPtr makeSrc(int w, int h) {
    auto b = ImageBuffer::create(w, h, PixelFormat::F32);
    float* p = reinterpret_cast<float*>(b->data().data());
    const size_t n = (size_t)w * h * 3;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t k = (uint32_t)(i * 2654435761u + 12345u);
        p[i] = 0.02f + 0.96f * (float)(k % 4093u) / 4092.0f;
    }
    return b;
}

static const int W = 64, H = 48;

static void hashMask(const char* name, const Mask& m) {
    std::vector<float> grid((size_t)W * H);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float nx = (x + 0.5f) / (float)W;
            const float ny = (y + 0.5f) / (float)H;
            grid[(size_t)y * W + x] = Pipeline::maskWeight(m, W, H, nx, ny);
        }
    }
    char k[64];
    std::snprintf(k, sizeof(k), "mask.weight.%s", name);
    emit(k, fnv(grid.data(), grid.size() * sizeof(float)));

    auto bw = Pipeline::brushWeightField(m, W, H);
    std::snprintf(k, sizeof(k), "mask.brushfield.%s", name);
    emit(k, bw ? fnv(bw->data(), bw->size() * sizeof(float)) : 0ull);

    float combined[W * H];
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float nx = (x + 0.5f) / (float)W;
            const float ny = (y + 0.5f) / (float)H;
            const float r = nx, g = ny, b = 0.5f * (nx + ny);
            combined[y * W + x] = Pipeline::combinedMaskWeight(m, W, H, nx, ny, r, g, b);
        }
    }
    std::snprintf(k, sizeof(k), "mask.combined.%s", name);
    emit(k, fnv(combined, sizeof(combined)));

    const uint64_t hashes[2] = { (uint64_t)m.hash(), (uint64_t)m.geometryHash() };
    std::snprintf(k, sizeof(k), "mask.hash.%s", name);
    emit(k, fnv(hashes, sizeof(hashes)));

    Adjustments a;
    a.masks.push_back(m);
    const std::string txt = ProfileIO::toJson(a).serialize(true);
    std::snprintf(k, sizeof(k), "mask.profile.%s", name);
    emit(k, fnv(txt.data(), txt.size()));
}

int main() {
    Mask linear;
    linear.type = MaskType::LinearGradient;
    linear.linear().startX = 0.1f; linear.linear().startY = 0.2f;
    linear.linear().endX = 0.9f;   linear.linear().endY = 0.7f;
    linear.linear().feather = 0.35f;
    linear.exposure = 0.4; linear.contrast = 12.0;
    hashMask("linear", linear);

    Mask radial;
    radial.type = MaskType::RadialGradient;
    radial.radial().centerX = 0.42f; radial.radial().centerY = 0.57f;
    radial.radial().radiusX = 0.3f;  radial.radial().radiusY = 0.22f;
    radial.radial().feather = 0.4f;  radial.radial().invert = true;
    radial.exposure = -0.3; radial.saturation = 8.0; radial.opacity = 0.8;
    hashMask("radial", radial);

    Mask brush;
    brush.type = MaskType::Brush;
    brush.brush().push_back(BrushDab{0.25f, 0.25f, 0.12f, 0.4f});
    brush.brush().push_back(BrushDab{0.55f, 0.45f, 0.08f, 0.6f});
    brush.brush().push_back(BrushDab{0.7f, 0.8f, 0.15f, 0.2f});
    brush.exposure = 0.5; brush.clarity = 10.0; brush.opacity = 0.9;
    hashMask("brush", brush);

    Mask color;
    color.type = MaskType::ColorRange;
    color.color().hasReference = true;
    color.color().refR = 0.35f; color.color().refG = 0.5f; color.color().refB = 0.2f;
    color.color().hueTolerance = 35.0;
    color.color().satTolerance = 0.45;
    color.color().lumaTolerance = 0.55;
    color.color().softness = 0.35;
    color.saturation = 25.0;
    hashMask("color", color);

    Mask combined;
    combined.type = MaskType::LinearGradient;
    combined.linear().startX = 0.0f; combined.linear().startY = 0.0f;
    combined.linear().endX = 1.0f;   combined.linear().endY = 1.0f;
    combined.linear().feather = 0.25f;
    combined.invert = true;
    combined.useColorRange = true;
    combined.colorRefR = 0.3f; combined.colorRefG = 0.6f; combined.colorRefB = 0.4f;
    combined.colorRange = 60.0f; combined.colorSmoothness = 40.0f;
    combined.useLuminance = true;
    combined.luminanceRef = 45.0f; combined.luminanceRange = 30.0f;
    combined.luminanceSmoothness = 70.0f;
    combined.maskUnion = true;
    combined.exposure = 0.2;
    hashMask("submask", combined);

    Mask ai;
    ai.type = MaskType::AiMask;
    ai.ai().target = AiMaskTarget::Subject;
    ai.ai().threshold = 0.45f;
    ai.ai().feather = 0.08f;
    ai.ai().invert = false;
    ai.exposure = 0.3;
    hashMask("ai", ai);

    // --- masked render path (local adjustments applied through the pipeline) ---
    auto src = makeSrc(320, 213);
    auto cm = std::make_shared<ColorManager>();
    {
        Pipeline p(cm);
        Adjustments a;
        a.exposure = 0.2;
        a.contrast = 10.0;
        a.masks.push_back(linear);
        a.masks.push_back(radial);
        a.masks.push_back(brush);
        a.masks.push_back(combined);
        auto f = p.renderPreview(*src, a, Size{320, 213}, 320, 21);
        emit("mask.render.localstack", hashBuf(*f));
    }
    {
        Pipeline p(cm);
        Adjustments a;
        a.masks.push_back(ai);
        auto f = p.renderPreview(*src, a, Size{320, 213}, 320, 22);
        emit("mask.render.ai", hashBuf(*f));
    }
    {
        auto w = Pipeline::aiMaskWeightField(ai, *src);
        emit("mask.ai.weightfield", w ? fnv(w->data(), w->size() * sizeof(float)) : 0ull);
    }
    // Sub-mask scalar helpers on a fixed sample.
    {
        const float samples[5][3] = {{0.1f,0.2f,0.3f},{0.9f,0.8f,0.7f},{0.5f,0.5f,0.5f},{0.0f,1.0f,0.0f},{0.33f,0.66f,0.99f}};
        float out[10];
        for (int i = 0; i < 5; ++i) {
            out[i * 2 + 0] = Pipeline::colorSubMaskWeight(combined, samples[i][0], samples[i][1], samples[i][2]);
            out[i * 2 + 1] = Pipeline::luminanceSubMaskWeight(combined, samples[i][0], samples[i][1], samples[i][2]);
        }
        emit("mask.subweights", fnv(out, sizeof(out)));
    }
    return 0;
}
