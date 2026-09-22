#pragma once

// Internal detail header (P3-4). Do NOT include from public engine headers.
//
// The legacy single-channel gaussian blur lives inside Pipeline.cpp's anonymous
// namespace (see gaussianBlur there). To let the spatial-filter backends reuse
// the exact same implementation without refactoring its internals, Pipeline.cpp
// exposes it through the `lrc::detail::gaussianBlur` bridge declared below.
// The bridge adds a single function-call hop and is byte-identical to calling
// the file-local function directly.

#include <vector>

namespace lrc {
namespace detail {

/**
 * @brief Separable gaussian blur of a single-channel image (bridge).
 *
 * Forwards to the Pipeline.cpp implementation. Contract (unchanged from the
 * legacy function): `out` is resized to w*h; sigma <= 0 copies the input;
 * sigma <= 4 runs the exact O(radius) separable convolution; larger sigma runs
 * three sliding-window box passes whose cascade approximates the Gaussian.
 * Edge samples are coordinate-clamped.
 *
 * @param src   Source luma/Y/Cb/Cr buffer, w*h floats.
 * @param w     Image width in pixels (> 0).
 * @param h     Image height in pixels (> 0).
 * @param sigma Gaussian sigma in pixels (>= 0).
 * @param out   Caller-owned output buffer; resized to w*h on entry.
 *
 * @note Thread-safe: the implementation uses thread-local scratch buffers only.
 */
void gaussianBlur(const float* src, int w, int h, float sigma,
                  std::vector<float>& out);

}  // namespace detail
}  // namespace lrc
