#pragma once

// Pure geometry helpers for cursor-anchored zoom/pan in the image viewport.
// Kept Qt-free so engine/catalog headless tests can exercise the math without
// pulling in any Qt headers.

namespace lrc {
namespace viewportmath {

/** @brief Zoomed pan offset (in widget px) that keeps the image point
 *         (imgX, imgY) under the widget cursor after scaling by `scale`.
 *
 * Derived by inverting the target-rect mapping:
 *   dst.left = viewportW / 2 + panX - imageW * scale / 2
 *   widgetX  = dst.left + imgX * scale == cursorX  =>  solve for panX.
 */
struct AnchorPan {
    double x = 0.0;
    double y = 0.0;
};

/** @brief Computes the pan offset that anchors the image point under the
 *         cursor for a given scale.
 *
 * @param cursorX/cursorY  Widget cursor position (px).
 * @param imgX/imgY        Image point in image pixel coordinates.
 * @param scale            Target display scale (> 0).
 * @param imageW/imageH    The rendered image size (px).
 * @param viewportW/viewportH  Widget viewport size (px).
 * @return The pan (x, y) to apply so the anchors coincide.
 */
inline AnchorPan anchorPan(double cursorX, double cursorY, double imgX, double imgY,
                           double scale, double imageW, double imageH,
                           double viewportW, double viewportH) {
    const double scaledW = imageW * scale;
    const double scaledH = imageH * scale;
    return {cursorX - imgX * scale + scaledW / 2.0 - viewportW / 2.0,
            cursorY - imgY * scale + scaledH / 2.0 - viewportH / 2.0};
}

/** @brief Whether a wheel zoom step should snap back to a fit mode.
 *
 * `dy` is the wheel delta (negative = zoom out). Snap-back only fires while
 * zooming OUT and when the post-step scale is at or below `fitScale * 1.05`.
 * The direction guard matters when the fit scale is already >= ~1.0 (native
 * image <= 1.05x the viewport): without it an upward wheel (zoom in) would
 * enter 1:1 at a scale already in the fit band and bounce straight back to
 * fit, silently killing wheel zoom-in.
 */
inline bool wheelShouldSnapBack(double dy, double currentScale, double fitScale) {
    return dy < 0 && currentScale <= fitScale * 1.05;
}

/** @brief Whether an image-space point lies inside the image.
 *
 * Accepts the half-open range [0, width) x [0, height), used to reject
 * letterbox clicks so a blank area cannot switch the viewport into 1:1 or
 * request the full-res decode.
 */
inline bool imagePointInBounds(double imgX, double imgY, double imageW, double imageH) {
    return imgX >= 0.0 && imgY >= 0.0 && imgX < imageW && imgY < imageH;
}

} // namespace viewportmath
} // namespace lrc
