#include "engine/ZoneCurve.h"

#include <algorithm>
#include <cmath>

namespace lrc {
namespace zonecurve {

std::vector<ToneCurvePoint> fromOutputs(float shadows, float darks,
                                        float lights, float highlights) {
    const auto ax = anchors();
    const float outs[kZoneCount] = {shadows, darks, lights, highlights};
    std::vector<ToneCurvePoint> out;
    out.reserve(kZoneCount + 2);
    out.push_back({0.0f, 0.0f});
    for (size_t i = 0; i < kZoneCount; ++i)
        out.push_back({ax[i], std::clamp(outs[i], 0.0f, 1.0f)});
    out.push_back({1.0f, 1.0f});
    return out;
}

std::vector<ToneCurvePoint> makeLinear() {
    const auto ax = anchors();
    return fromOutputs(ax[0], ax[1], ax[2], ax[3]);
}

std::vector<ToneCurvePoint> fromPoints(
    const std::vector<ToneCurvePoint>& points) {
    const auto ax = anchors();
    // Per-zone output values; linear (y == anchor x) until claimed by a point.
    std::array<float, kZoneCount> ys = ax;
    std::array<float, kZoneCount> bestDist;
    bestDist.fill(1e9f);
    for (const ToneCurvePoint& p : points) {
        size_t best = 0;
        float bestd = 1e9f;
        for (size_t i = 0; i < kZoneCount; ++i) {
            const float d = std::abs(p.x - ax[i]);
            if (d < bestd) {
                bestd = d;
                best = i;
            }
        }
        if (bestd < bestDist[best]) {
            bestDist[best] = bestd;
            ys[best] = std::clamp(p.y, 0.0f, 1.0f);
        }
    }
    return fromOutputs(ys[0], ys[1], ys[2], ys[3]);
}

}  // namespace zonecurve
}  // namespace lrc
