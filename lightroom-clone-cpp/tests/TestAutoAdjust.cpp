#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>

#include "engine/ImageBuffer.h"
#include "engine/Pipeline.h"

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

// Fills a fresh F32 buffer with two colours: the top `firstFrac` fraction of
// pixels (row-major) get (r1,g1,b1); the rest get (r2,g2,b2).
std::shared_ptr<ImageBuffer> makeTwoTone(int w, int h, float r1, float g1,
                                         float b1, float r2, float g2, float b2,
                                         double firstFrac) {
    auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
    float* p = reinterpret_cast<float*>(buf->data().data());
    const size_t n = static_cast<size_t>(w) * h;
    const size_t first = static_cast<size_t>(n * firstFrac);
    for (size_t i = 0; i < n; ++i) {
        const bool top = i < first;
        p[i * 3 + 0] = top ? r1 : r2;
        p[i * 3 + 1] = top ? g1 : g2;
        p[i * 3 + 2] = top ? b1 : b2;
    }
    return buf;
}

} // namespace

TEST_CASE("AutoAdjust: mid-grey image needs no correction", "[autoadjust]") {
    // 全图 (0.18,0.18,0.18): 中灰 -> exposure/temperature/tint 都归零.
    auto buf = makeFlat(64, 64, 0.18f, 0.18f, 0.18f);
    const Adjustments a = Pipeline::autoAdjust(*buf);
    REQUIRE(a.exposure == Approx(0.0).margin(0.05));
    REQUIRE(a.temperature == Approx(0.0).margin(1e-6));
    REQUIRE(a.tint == Approx(0.0).margin(1e-6));
}

TEST_CASE("AutoAdjust: dark image lifts exposure", "[autoadjust]") {
    // 全图 (0.09,...): 偏暗 -> exposure>0, log2(0.18/0.09)=1.0, 死区 0.05 不拦.
    auto buf = makeFlat(64, 64, 0.09f, 0.09f, 0.09f);
    const Adjustments a = Pipeline::autoAdjust(*buf);
    REQUIRE(a.exposure > 0.0);
    REQUIRE(a.exposure == Approx(1.0).margin(0.1));
}

TEST_CASE("AutoAdjust: bright image lowers exposure", "[autoadjust]") {
    // 全图 (0.36,...): 偏亮 -> exposure<0, log2(0.18/0.36)=-1.0.
    auto buf = makeFlat(64, 64, 0.36f, 0.36f, 0.36f);
    const Adjustments a = Pipeline::autoAdjust(*buf);
    REQUIRE(a.exposure < 0.0);
    REQUIRE(a.exposure == Approx(-1.0).margin(0.1));
}

TEST_CASE("AutoAdjust: warm cast gives a negative temperature", "[autoadjust]") {
    // 暖偏图: 全图 (0.6,0.5,0.4), R>B -> 负色温 (warm cast -> negative).
    // 对照 estimateWhiteBalance 推导: t = log2(lb/lr)/0.012 < 0.
    auto buf = makeFlat(64, 64, 0.6f, 0.5f, 0.4f);
    const Adjustments a = Pipeline::autoAdjust(*buf);
    REQUIRE(a.temperature < 0.0);
}

TEST_CASE("AutoAdjust: clipped highlights are pulled down", "[autoadjust]") {
    // 高亮剪贴: 80% 像素 (0.99,...) + 20% (0.2,...) -> highlights <= 0.
    auto buf = makeTwoTone(100, 100, 0.99f, 0.99f, 0.99f, 0.2f, 0.2f, 0.2f, 0.8);
    const Adjustments a = Pipeline::autoAdjust(*buf);
    REQUIRE(a.highlights <= 0.0);
}

TEST_CASE("AutoAdjust: shallow shadows raise the black point", "[autoadjust]") {
    // 黑点抬升: 全图 (0.10,...) 最低亮度 0.10 > 0.04 -> blacks > 0.
    auto buf = makeFlat(64, 64, 0.10f, 0.10f, 0.10f);
    const Adjustments a = Pipeline::autoAdjust(*buf);
    REQUIRE(a.blacks > 0.0);
}

TEST_CASE("AutoAdjust: empty buffer returns defaults", "[autoadjust]") {
    // 空缓冲 -> 返回全默认 (defaults), 所有自动字段为 0, curve 为空.
    const Adjustments a = Pipeline::autoAdjust(ImageBuffer{});
    REQUIRE(a == Adjustments::defaults());
    REQUIRE(a.exposure == 0.0);
    REQUIRE(a.contrast == 0.0);
    REQUIRE(a.highlights == 0.0);
    REQUIRE(a.shadows == 0.0);
    REQUIRE(a.whites == 0.0);
    REQUIRE(a.blacks == 0.0);
    REQUIRE(a.temperature == 0.0);
    REQUIRE(a.tint == 0.0);
    REQUIRE(a.curve.empty());
}

TEST_CASE("AutoAdjust: deterministic for identical input", "[autoadjust]") {
    // 确定性: 同一缓冲两次 -> 结果相等且 hash 相等.
    auto buf = makeTwoTone(80, 80, 0.8f, 0.5f, 0.3f, 0.1f, 0.1f, 0.1f, 0.5);
    const Adjustments a = Pipeline::autoAdjust(*buf);
    const Adjustments b = Pipeline::autoAdjust(*buf);
    REQUIRE(a == b);
    REQUIRE(a.hash() == b.hash());
}

TEST_CASE("AutoAdjust: extreme images stay inside slider ranges", "[autoadjust]") {
    // clamp 不越界: 全黑 (0,0,0) 与全白 (1,1,1) 的每个自动字段都不越界.
    const auto black = makeFlat(64, 64, 0.0f, 0.0f, 0.0f);
    const auto white = makeFlat(64, 64, 1.0f, 1.0f, 1.0f);
    const Adjustments dark = Pipeline::autoAdjust(*black);
    const Adjustments bright = Pipeline::autoAdjust(*white);

    const auto check = [](const Adjustments& a) {
        REQUIRE(a.exposure >= -3.0);
        REQUIRE(a.exposure <= 3.0);
        REQUIRE(a.contrast >= -50.0);
        REQUIRE(a.contrast <= 50.0);
        REQUIRE(a.highlights >= -100.0);
        REQUIRE(a.highlights <= 0.0);
        REQUIRE(a.shadows >= 0.0);
        REQUIRE(a.shadows <= 100.0);
        REQUIRE(a.whites >= 0.0);
        REQUIRE(a.whites <= 100.0);
        REQUIRE(a.blacks >= 0.0);
        REQUIRE(a.blacks <= 100.0);
        REQUIRE(a.temperature >= -100.0);
        REQUIRE(a.temperature <= 100.0);
        REQUIRE(a.tint >= -100.0);
        REQUIRE(a.tint <= 100.0);
        // 若曲线非空, 每点坐标都在 [0,1].
        for (const auto& pt : a.curve) {
            REQUIRE(pt.x >= 0.0f);
            REQUIRE(pt.x <= 1.0f);
            REQUIRE(pt.y >= 0.0f);
            REQUIRE(pt.y <= 1.0f);
        }
    };
    check(dark);
    check(bright);
}

TEST_CASE("AutoAdjust: low-contrast image emits a rising tone curve",
          "[autoadjust]") {
    // 低对比图 (span 小, s 大): 80% (0.2,...) + 20% (0.3,...),
    // span ~= 0.1 -> s = (0.45-0.1)*0.06 ~= 0.021 > 0.01 -> 曲线非空.
    auto buf = makeTwoTone(100, 100, 0.2f, 0.2f, 0.2f, 0.3f, 0.3f, 0.3f, 0.8);
    const Adjustments a = Pipeline::autoAdjust(*buf);
    REQUIRE(!a.curve.empty());
    REQUIRE(a.curve.size() >= 2u);
    for (const auto& pt : a.curve) {
        REQUIRE(pt.x >= 0.0f);
        REQUIRE(pt.x <= 1.0f);
        REQUIRE(pt.y >= 0.0f);
        REQUIRE(pt.y <= 1.0f);
    }
    for (size_t i = 1; i < a.curve.size(); ++i) {
        REQUIRE(a.curve[i].x > a.curve[i - 1].x);   // x 严格递增 (strictly increasing).
        REQUIRE(a.curve[i].y >= a.curve[i - 1].y);  // y 单调不减 (non-decreasing).
    }
}

TEST_CASE("AutoAdjust: high-contrast image emits no tone curve", "[autoadjust]") {
    // 高对比图 (span 大, s<=0.01): 50% 黑 + 50% 白, span=1.0 ->
    // s = (0.45-1.0)*0.06 = -0.033 <= 0.01 -> 曲线为空.
    auto buf = makeTwoTone(100, 100, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.5);
    const Adjustments a = Pipeline::autoAdjust(*buf);
    REQUIRE(a.curve.empty());
}
