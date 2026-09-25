#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <vector>

#include "engine/ImageBuffer.h"
#include "engine/PregradeAnalyzer.h"

using namespace Catch;
using namespace lrc;

namespace {

// Fills every pixel of a fresh F32 buffer with a constant RGB value.
std::shared_ptr<ImageBuffer> makeFlat(int w, int h, float r, float g, float b) {
    auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
    float* p = reinterpret_cast<float*>(buf->data().data());
    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n; ++i) {
        p[i * 3 + 0] = r;
        p[i * 3 + 1] = g;
        p[i * 3 + 2] = b;
    }
    return buf;
}

// Fills a fresh F32 buffer with a bright central rectangle over a dark base.
// Returns a buffer where the central `fracFrac`-of-width rectangle is (r1,g1,b1)
// and everything else is (r2,g2,b2) — a central-subject stress pattern.
std::shared_ptr<ImageBuffer> makeCentral(int w, int h, float r1, float g1,
                                         float b1, float r2, float g2, float b2,
                                         double fracFrac) {
    auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
    float* p = reinterpret_cast<float*>(buf->data().data());
    const int x0 = static_cast<int>((1.0 - fracFrac) * 0.5 * w);
    const int x1 = static_cast<int>((1.0 + fracFrac) * 0.5 * w);
    const int y0 = static_cast<int>((1.0 - fracFrac) * 0.5 * h);
    const int y1 = static_cast<int>((1.0 + fracFrac) * 0.5 * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const bool inside = (x >= x0 && x < x1 && y >= y0 && y < y1);
            p[i * 3 + 0] = inside ? r1 : r2;
            p[i * 3 + 1] = inside ? g1 : g2;
            p[i * 3 + 2] = inside ? b1 : b2;
        }
    }
    return buf;
}

} // namespace

TEST_CASE("Pregrade: empty buffer is not processed", "[ai][pregrade]") {
    const PregradeResult r = PregradeAnalyzer::pregrade(ImageBuffer{}, nullptr);
    REQUIRE_FALSE(r.processed);
    REQUIRE_FALSE(r.hasSubject);
}

TEST_CASE("Pregrade: uniform image has no subject and a bounded score", "[ai][pregrade]") {
    auto buf = makeFlat(96, 96, 0.18f, 0.18f, 0.18f);
    const PregradeResult r = PregradeAnalyzer::pregrade(*buf, nullptr);
    REQUIRE(r.processed);
    REQUIRE_FALSE(r.hasSubject);           // uniform -> no central saliency
    REQUIRE(r.score >= 0);
    REQUIRE(r.score <= 100);
}

TEST_CASE("Pregrade: central bright block flags a subject", "[ai][pregrade]") {
    // 中央亮块 + 暗底 -> 中央显著性应标记 hasSubject.
    auto buf = makeCentral(128, 128, 0.9f, 0.9f, 0.9f, 0.03f, 0.03f, 0.03f, 0.3);
    const PregradeResult r = PregradeAnalyzer::pregrade(*buf, nullptr);
    REQUIRE(r.processed);
    REQUIRE(r.hasSubject);
    REQUIRE(r.score > 0);
}

TEST_CASE("Pregrade: dark image suggests auto-develop", "[ai][pregrade]") {
    // 全图暗 -> exposure 需要抬升 -> suggestAuto=true.
    auto buf = makeFlat(64, 64, 0.05f, 0.05f, 0.05f);
    const PregradeResult r = PregradeAnalyzer::pregrade(*buf, nullptr);
    REQUIRE(r.processed);
    REQUIRE(r.suggestAuto);
}

TEST_CASE("Pregrade: missing model does not block the batch", "[ai][pregrade]") {
    // AC-4: engine=null (无模型) 时仍走统计路径, 不崩溃、processed=true.
    auto buf = makeCentral(96, 96, 0.8f, 0.6f, 0.4f, 0.05f, 0.05f, 0.05f, 0.25);
    const PregradeResult r = PregradeAnalyzer::pregrade(*buf, nullptr);
    REQUIRE(r.processed);
    // hasSubject 由确定性兜底决定; 这里只要求不崩溃且分数有界.
    REQUIRE(r.score >= 0);
    REQUIRE(r.score <= 100);
}

TEST_CASE("Pregrade: non-destructive (source bytes unchanged)", "[ai][pregrade]") {
    // AC-2: 分析不得原地改写输入缓冲.
    auto buf = makeCentral(80, 80, 0.7f, 0.5f, 0.3f, 0.02f, 0.02f, 0.02f, 0.3);
    const std::vector<std::byte> before(
        reinterpret_cast<std::byte*>(buf->data().data()),
        reinterpret_cast<std::byte*>(buf->data().data()) +
            static_cast<long long>(buf->data().size()));
    (void)PregradeAnalyzer::pregrade(*buf, nullptr);
    const std::vector<std::byte> after(
        reinterpret_cast<std::byte*>(buf->data().data()),
        reinterpret_cast<std::byte*>(buf->data().data()) +
            static_cast<long long>(buf->data().size()));
    REQUIRE(before == after);
}

TEST_CASE("Pregrade: deterministic for identical input", "[ai][pregrade]") {
    auto buf = makeCentral(100, 100, 0.8f, 0.6f, 0.4f, 0.04f, 0.04f, 0.04f, 0.3);
    const PregradeResult a = PregradeAnalyzer::pregrade(*buf, nullptr);
    const PregradeResult b = PregradeAnalyzer::pregrade(*buf, nullptr);
    REQUIRE(a.hasSubject == b.hasSubject);
    REQUIRE(a.score == b.score);
    REQUIRE(a.suggestAuto == b.suggestAuto);
    REQUIRE(a.processed == b.processed);
}
