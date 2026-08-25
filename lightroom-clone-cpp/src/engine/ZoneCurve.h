#pragma once

// Lightroom-style zone tone curve. A single RGB curve is built from four
// draggable zone handles - shadows / darks / lights / highlights - pinned
// between the fixed black and white endpoints. It is stored as a plain
// vector<ToneCurvePoint>, so the pipeline, JSON serialization and the preview
// cache are all unchanged; these helpers centralize the zone layout for the
// CurveEditor widget and stay testable in the headless build.
//
// interpolate() is the monotone piecewise-cubic (Fritsch-Carlson) evaluation
// shared by the pipeline's LUT and the editor's drawing, so what the user sees
// in the widget is exactly what the renderer applies. Unlike Catmull-Rom it
// reproduces linear data exactly (the identity curve is a straight diagonal)
// and never overshoots between control points.

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "engine/Adjustments.h"

namespace lrc {
namespace zonecurve {

constexpr size_t kZoneCount = 4;

// Anchor x (input tonal position) of each zone, in shadows..highlights order.
inline std::array<float, kZoneCount> anchors() {
    return {0.25f, 0.47f, 0.64f, 0.82f};
}

// Curve points (black endpoint, four zones, white endpoint) from explicit zone
// outputs, in shadows..highlights order. Outputs are clamped to [0,1].
std::vector<ToneCurvePoint> fromOutputs(float shadows, float darks,
                                        float lights, float highlights);

// Identity curve: every zone output equals its anchor, giving y == x.
std::vector<ToneCurvePoint> makeLinear();

// Quantizes an arbitrary point set onto the four zone handles by nearest
// anchor x. Zones with no nearby point stay linear. Used when loading curves
// produced by the older free-form editor.
std::vector<ToneCurvePoint> fromPoints(const std::vector<ToneCurvePoint>& points);

// Monotone piecewise-cubic interpolation of the (x-sorted) points, evaluated
// at normalized input x in [0,1]. Falls back to the nearest endpoint when the
// domain is degenerate.
inline float interpolate(const std::vector<ToneCurvePoint>& points, float x) {
    const size_t n = points.size();
    if (n == 0)
        return x;
    if (n == 1)
        return points[0].y;
    if (n == 2) {
        const float span = points[1].x - points[0].x;
        const float u = span > 1e-6f
                            ? std::clamp((x - points[0].x) / span, 0.0f, 1.0f)
                            : 0.0f;
        return points[0].y + (points[1].y - points[0].y) * u;
    }

    x = std::clamp(x, points.front().x, points.back().x);

    // Find the segment i with points[i].x <= x <= points[i+1].x.
    size_t i = 0;
    for (i = 0; i + 1 < n; ++i) {
        if (x <= points[i + 1].x)
            break;
    }
    const float h = points[i + 1].x - points[i].x;
    if (h <= 1e-6f)
        return points[i].y;

    // Secant slope of segment k.
    auto secant = [&](size_t k) -> float {
        const float hh = points[k + 1].x - points[k].x;
        return hh > 1e-6f ? (points[k + 1].y - points[k].y) / hh : 0.0f;
    };

    // Monotonicity-preserving derivative at node k (Fritsch-Carlson).
    auto deriv = [&](size_t k) -> float {
        if (k == 0) {
            const float s0 = secant(0);
            if (s0 <= 0.0f)
                return 0.0f;
            float d = s0;
            if (n >= 3) {
                const float h0 = points[1].x - points[0].x;
                const float h1 = points[2].x - points[1].x;
                d = ((2.0f * h0 + h1) * s0 - h0 * secant(1)) / (h0 + h1);
            }
            return std::clamp(d, 0.0f, 3.0f * s0);
        }
        if (k == n - 1) {
            const float sn = secant(n - 2);
            if (sn <= 0.0f)
                return 0.0f;
            float d = sn;
            if (n >= 3) {
                const float h1 = points[n - 2].x - points[n - 3].x;
                const float h2 = points[n - 1].x - points[n - 2].x;
                d = ((2.0f * h2 + h1) * sn - h2 * secant(n - 3)) / (h1 + h2);
            }
            return std::clamp(d, 0.0f, 3.0f * sn);
        }
        const float a = secant(k - 1);
        const float b = secant(k);
        if (a * b <= 0.0f)
            return 0.0f;  // local extremum: flatten the derivative
        return 2.0f * a * b / (a + b);  // harmonic mean
    };

    const float y0 = points[i].y;
    const float y1 = points[i + 1].y;
    const float d0 = deriv(i);
    const float d1 = deriv(i + 1);
    const float u = (x - points[i].x) / h;
    const float u2 = u * u;
    const float u3 = u2 * u;
    // Cubic Hermite basis on [0,1].
    const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
    const float h10 = u3 - 2.0f * u2 + u;
    const float h01 = -2.0f * u3 + 3.0f * u2;
    const float h11 = u3 - u2;
    return y0 * h00 + h * d0 * h10 + y1 * h01 + h * d1 * h11;
}

}  // namespace zonecurve
}  // namespace lrc
