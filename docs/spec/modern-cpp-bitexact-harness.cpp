
// Bit-exactness oracle for the modern-C++ adoption refactor.
// Renders deterministic synthetic content through public engine APIs and
// prints a 64-bit FNV-1a hash per case. Run before/after a refactor and diff
// the two outputs; every line must be identical.
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <memory>

#include "engine/Adjustments.h"
#include "engine/ColorManager.h"
#include "engine/HdrMerge.h"
#include "engine/ImageBuffer.h"
#include "engine/Pipeline.h"
#include "utils/Json.h"
#include "utils/Parallel.h"
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
static Adjustments adjA() {
    Adjustments a;
    a.exposure = 0.42; a.contrast = 21.0; a.highlights = -18.0; a.shadows = 14.0;
    a.whites = 9.0; a.blacks = -7.0; a.temperature = 5.0; a.tint = -3.0;
    a.saturation = 12.0; a.vibrance = 8.0; a.clarity = 6.0; a.texture = 4.0;
    a.sharpness = 22.0; a.grain = 12.0; a.grainSize = 30.0;
    return a;
}
static Adjustments adjB() {
    Adjustments a = adjA();
    a.hsl[(size_t)ColorRange::Red].saturation = 17.0;
    a.hsl[(size_t)ColorRange::Blue].hue = -9.0;
    return a;
}

int main() {
    // ---- ImageBuffer geometry / format ops ------------------------------
    auto src = makeSrc(640, 427);
    emit("ib.src", hashBuf(*src));
    { auto t = src->resizeToF32(320, 213); emit("ib.resizeToF32", hashBuf(*t)); }
    { auto t = src->resize(320, 213); emit("ib.resize", hashBuf(*t)); }
    { auto t = src->toU16(); emit("ib.toU16", hashBuf(*t)); }
    { auto t = src->crop(17, 23, 400, 300); emit("ib.crop", hashBuf(*t)); }
    { auto t = ImageBuffer::create(64, 64, PixelFormat::U8);
      for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
          for (int c = 0; c < 3; ++c) t->setPixel(x, y, c, (x + y + c * 13) / 200.0f);
      emit("ib.setPixel", hashBuf(*t));
      emit("ib.toF32fromU8", hashBuf(*t->toF32())); }

    // ---- ColorManager ---------------------------------------------------
    auto cm = std::make_shared<ColorManager>();
    { auto d = cm->toDisplaySRGB(*src); emit("cm.toDisplaySRGB", hashBuf(*d)); }
    { std::vector<float> f(3 * 1000), o(3 * 1000);
      for (int i = 0; i < 3000; ++i) f[i] = (float)((i * 7919) % 1000) / 999.0f;
      cm->linearProPhotoToSRGB(f.data(), o.data(), 1000);
      emit("cm.linearToSRGB", fnv(o.data(), o.size() * sizeof(float))); }
    { uint8_t r = 12, g = 200, b = 77; float lr = 0, lg = 0, lb = 0;
      cm->srgbPixelToLinearProPhoto(r, g, b, lr, lg, lb);
      float v[3] = { lr, lg, lb }; emit("cm.srgbPixelToLinear", fnv(v, sizeof(v))); }

    // ---- Pipeline: preview / incremental / full ------------------------
    { Pipeline p(cm);
      auto f1 = p.renderPreview(*src, adjA(), Size{320, 213}, 640, 11);
      emit("pl.preview.a", hashBuf(*f1));
      auto f2 = p.renderPreview(*src, adjB(), Size{320, 213}, 640, 11);
      emit("pl.preview.b", hashBuf(*f2));
      auto f3 = p.renderPreview(*src, adjB(), Size{320, 213}, 640, 11);
      emit("pl.preview.b.hit", hashBuf(*f3));
      ImageBufferPtr lin;
      p.clearIncrementalCache();
      auto f4 = p.renderPreview(*src, adjA(), Size{320, 213}, 640, 12, &lin);
      emit("pl.preview.a2", hashBuf(*f4));
      if (lin) emit("pl.preview.a2.linear", hashBuf(*lin)); }
    { Pipeline p(cm);
      auto f = p.renderFullDisplay(0, *src, adjA());
      emit("pl.fullDisplay", hashBuf(*f)); }
    { auto h = Pipeline::computeHistogramFromLinear(*src);
      emit("pl.histogram", fnv(&h, sizeof(h))); }

    // ---- HdrMerge -------------------------------------------------------
    { auto s1 = makeSrc(320, 213);
      auto s2 = ImageBuffer::create(320, 213, PixelFormat::F32);
      const float* a = reinterpret_cast<const float*>(s1->data().data());
      float* b = reinterpret_cast<float*>(s2->data().data());
      for (size_t i = 0; i < (size_t)320 * 213 * 3; ++i) b[i] = a[i] * 1.9f + 0.01f;
      emit("hdr.estimateEv*1e6", (uint64_t)(int64_t)llround(HdrMerge::estimateEv(*s1, *s2) * 1e6));
      auto r1 = HdrMerge::merge({s1, s2}, {}, false, 64);
      emit("hdr.merge.band64", r1.ok() ? hashBuf(*r1.linear) : 0ull);
      auto r2 = HdrMerge::merge({s1, s2}, {}, false, 4096);
      emit("hdr.merge.band4096", r2.ok() ? hashBuf(*r2.linear) : 0ull);
      auto r3 = HdrMerge::merge({s1, s2}, {0.0, -1.0}, true, 45);
      emit("hdr.merge.deghost", r3.ok() ? hashBuf(*r3.linear) : 0ull); }

    // ---- Parallel determinism -----------------------------------------
    { const size_t n = 200000;
      std::vector<float> out(n, 0.0f);
      parallel::parallelFor(n, parallel::defaultGrain(n), [&](size_t b, size_t e) {
          for (size_t i = b; i < e; ++i) out[i] = (float)((i * 2654435761u) % 997u) / 997.0f;
      });
      emit("par.fill", fnv(out.data(), out.size() * sizeof(float))); }

    // ---- Json / ProfileIO ---------------------------------------------
    { auto root = json::Value::object();
      root["i"] = json::Value(1234567890123LL);
      root["d"] = json::Value(0.1 + 0.2);
      root["s"] = json::Value(std::string("h\xc3\xa9llo world"));
      root["b"] = json::Value(true);
      auto arr = json::Value::array();
      for (int i = 0; i < 12; ++i) arr.push_back(json::Value(i * 0.5));
      root["a"] = arr;
      const std::string compact = root.serialize(false);
      const std::string pretty = root.serialize(true);
      emit("json.compact", fnv(compact.data(), compact.size()));
      emit("json.pretty", fnv(pretty.data(), pretty.size()));
      auto back = json::Value::parse(compact);
      const std::string again = back ? back->serialize(false) : std::string("parse-failed");
      emit("json.roundtrip", fnv(again.data(), again.size())); }
    { const std::string p = ProfileIO::toJson(adjB()).serialize(true);
      emit("profile.serialize", fnv(p.data(), p.size()));
      auto back = json::Value::parse(ProfileIO::toJson(adjB()).serialize(false));
      const std::string p2 = back ? ProfileIO::toJson(ProfileIO::fromJson(*back)).serialize(true) : std::string("x");
      emit("profile.roundtrip", fnv(p2.data(), p2.size())); }
    return 0;
}