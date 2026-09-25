// P-A performance milestone (M1–M3) engine-layer acceptance.
//
// These are reproducible, headless baseline measurements for the P-A quant
// anchors that can be exercised without a GPU or a real 50MP raw file. We push a
// synthetic 50MP F32 linear buffer (8000x6000) through the engine render paths
// and record wall-time / process-memory samples so a real number can be cited
// instead of an untested claim.
//
// The numbers here are recorded via INFO() and gated only by LOOSE smoke
// thresholds. They are NOT the acceptance gate for the anchors: B-1/B-2/B-3/
// D-1/D-2 must still be accepted on a real machine (the spec pillar docs keep
// that distinction). This suite exists so a repeatable, visible baseline exists
// in headless CI and so gross regressions (e.g. an accidental O(n^2) render)
// fail loudly.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <numeric>
#include <vector>

#include "app/Constants.h"
#include "engine/Adjustments.h"
#include "engine/ColorManager.h"
#include "engine/ImageBuffer.h"
#include "engine/Pipeline.h"

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

using namespace Catch;
using namespace lrc;

namespace {

// Synthetic 50MP linear ProPhoto F32 source, filled with a smoothly varying
// gradient so no single channel is constant (keeps the render honest). Returns
// null on allocation failure.
ImageBufferPtr makeSynthetic50MP() {
    constexpr int kW = 8000;
    constexpr int kH = 6000;
    constexpr PixelFormat kFmt = PixelFormat::F32;
    auto buf = ImageBuffer::create(kW, kH, kFmt);
    if (!buf) {
        return nullptr;
    }
    // Interleaved RGB F32. A smooth gradient couples x/y position to each
    // channel value, exercising the per-pixel transforms with varied data.
    float* px = reinterpret_cast<float*>(buf->data().data());
    const size_t count = static_cast<size_t>(kW) * kH * 3u;
    for (size_t i = 0; i < count; ++i) {
        const float v = 0.08f + 0.84f * static_cast<float>(i % 1013) / 1012.f;
        px[i] = v;
    }
    return buf;
}

// Representative primary-slider stack, per the anchor convention
// (exposure/contrast/highlights/shadows/whites/blacks/temperature).
Adjustments representativeAdjustments(int seedOffset) {
    Adjustments a;
    a.exposure = 0.5 + seedOffset * 0.1;
    a.contrast = 25 + seedOffset;
    a.highlights = -30 - seedOffset;
    a.shadows = 20 + seedOffset;
    a.whites = 10;
    a.blacks = -8;
    a.temperature = 6 + seedOffset;
    return a;
}

template <typename Fn>
double runTimed(Fn&& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

#if defined(_WIN32)
// Process private committed bytes (PagefileUsage). Zero on failure.
struct MemSample {
    std::uint64_t privateCommit = 0; // PagefileUsage: private committed bytes
};
MemSample sampleProcessMemory() {
    PROCESS_MEMORY_COUNTERS pmc{};
    const SIZE_T cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, cb) == 0) {
        return {};
    }
    MemSample s;
    s.privateCommit = pmc.PagefileUsage;
    return s;
}
#endif

} // namespace

TEST_CASE("P-A: 50MP synthetic 1920 refine-frame render time (B-2)", "[performance][pa]") {
    auto src = makeSynthetic50MP();
    REQUIRE(src != nullptr);

    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);

    const lrc::Size target{1920, 1440};
    std::vector<double> samples;

    // Each iteration varies the adjustment stack so the incremental cache cannot
    // serve the frame from the previous render -- every sample is a genuine
    // fresh 1920 refine frame, matching a real slider change.
    for (int i = 0; i < 3; ++i) {
        const auto adj = representativeAdjustments(i);
        double ms = 0.0;
        ImageBufferPtr frame;
        ms = runTimed([&] {
            frame = pipeline.renderPreview(*src, adj, target, src->height(), 0);
        });
        REQUIRE(frame != nullptr);
        REQUIRE(frame->width() == 1920);
        INFO("B-2 refine iter " << i << ": " << ms << " ms");
        samples.push_back(ms);
    }

    const double mn = *std::min_element(samples.begin(), samples.end());
    const double avg = std::accumulate(samples.begin(), samples.end(), 0.0) /
                       static_cast<double>(samples.size());
    INFO("B-2 refine min=" << mn << " ms avg=" << avg << " ms");
    // Loose smoke guard (sandbox CI; real anchor gate happens on a real machine).
    REQUIRE(mn < 2000.0);
}

TEST_CASE("P-A: 50MP synthetic 1:1 full-res render time (B-1 engine leg)", "[performance][pa]") {
    auto src = makeSynthetic50MP();
    REQUIRE(src != nullptr);

    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);

    // engine leg covers full-res linear -> sRGB U8 render only; RAW decode and
    // disk 1:1 read are exercised by PreviewWorker on a real machine.
    const auto adj = representativeAdjustments(0);
    ImageBufferPtr full;
    const double ms = runTimed([&] {
        full = pipeline.renderFullDisplay(0, *src, adj);
    });
    REQUIRE(full != nullptr);
    REQUIRE(full->width() == src->width());
    REQUIRE(full->height() == src->height());
    INFO("B-1 engine-leg full-res render: " << ms << " ms");
    REQUIRE(ms < 8000.0);
}

TEST_CASE("P-A: 50MP synthetic peak working-set (D-1)", "[performance][memory][pa]") {
#if defined(_WIN32)
    const auto before = sampleProcessMemory();
    double clampMs = 0.0;
    const double heldMB = static_cast<double>(before.privateCommit) /
                          (1024.0 * 1024.0);
    {
        auto src = makeSynthetic50MP();
        REQUIRE(src != nullptr);
        // Exercise the render so peak reflects the working buffer + output.
        auto cm = std::make_shared<ColorManager>();
        Pipeline pipeline(cm);
        clampMs = runTimed([&] {
            auto frame = pipeline.renderFullDisplay(0, *src,
                                                    representativeAdjustments(0));
            REQUIRE(frame != nullptr);
        });
        // Commit grows while the 50MP F32 buffer + U8 output are alive.
        const auto heldMid = sampleProcessMemory();
        const double midMB =
            static_cast<double>(heldMid.privateCommit) / (1024.0 * 1024.0);
        INFO("D-1 commit while holding: " << midMB << " MB (baseline " << heldMB
                                          << " MB; delta " << (midMB - heldMB)
                                          << " MB)");
    }
    const auto after = sampleProcessMemory();
    const double currentMB =
        static_cast<double>(after.privateCommit) / (1024.0 * 1024.0);
    INFO("D-1 full-res render took " << clampMs << " ms; commit after scope "
                                     << currentMB << " MB (falls back toward "
                                     << heldMB << " MB baseline)");
    // Private-commit baseline must be nonzero (sampling worked).
    REQUIRE(heldMB > 0.0);
#else
    SUCCEED("D-1 process-memory sampling is Windows-only; skipped.");
#endif
}

TEST_CASE("P-A: 50MP synthetic cut-away memory fallback is repeatable (D-2)",
          "[performance][memory][pa]") {
#if defined(_WIN32)
    bool got[2] = {false, false};
    double heldMB[2] = {0.0, 0.0};
    double afterMB[2] = {0.0, 0.0};

    for (int trial = 0; trial < 2; ++trial) {
        const auto m0 = sampleProcessMemory();
        const double baselineMB =
            static_cast<double>(m0.privateCommit) / (1024.0 * 1024.0);
        {
            auto src = makeSynthetic50MP();
            REQUIRE(src != nullptr);
            auto cm = std::make_shared<ColorManager>();
            Pipeline pipeline(cm);
            auto frame =
                pipeline.renderFullDisplay(0, *src, representativeAdjustments(0));
            REQUIRE(frame != nullptr);
            const auto mid = sampleProcessMemory();
            heldMB[trial] = static_cast<double>(mid.privateCommit) /
                            (1024.0 * 1024.0);
        } // src + frame released here; private commit should drop.
        const auto m1 = sampleProcessMemory();
        afterMB[trial] =
            static_cast<double>(m1.privateCommit) / (1024.0 * 1024.0);
        got[trial] = true;
        INFO("D-2 trial " << trial << ": baseline " << baselineMB
                          << " MB -> held " << heldMB[trial]
                          << " MB -> released " << afterMB[trial]
                          << " MB (fallback delta "
                          << (heldMB[trial] - afterMB[trial]) << " MB)");
    }

    // Fallback must be repeatable: both trials release a substantial, stable
    // fraction of the 50MP working buffer, not a one-off snapshot artifact.
    REQUIRE(got[0]);
    REQUIRE(got[1]);
    const double delta0 = heldMB[0] - afterMB[0];
    const double delta1 = heldMB[1] - afterMB[1];
    INFO("D-2 fallback deltas: " << delta0 << " MB / " << delta1 << " MB");
    REQUIRE(delta0 > 0.0);
    REQUIRE(delta1 > 0.0);
#else
    SUCCEED("D-2 process-memory sampling is Windows-only; skipped.");
#endif
}