#pragma once

// Pure C++ panorama stitching for the engine layer. Qt-free.
//
// stitchPanorama() takes an ordered sequence of overlapping images (typically a
// horizontal sweep of landscape photos) and blends them into a single panorama
// buffer. The pipeline is:
//
//   1. translation alignment -- each input is area-average downscaled to a
//      common max edge and adjacent pairs are aligned by spatial-domain
//      normalized cross-correlation (NCC) on the luminance,
//   2. optional cylindrical projection -- each image is warped to a common
//      focal-length cylinder before compositing (pure translation mode skips
//      this and treats the inputs as a flat mosaic),
//   3. feather-weighted blending -- per-pixel weights ramp from 0 at each
//      source image's boundary to 1 in its interior; overlapping pixels are
//      averaged. The blend streams in row bands to bound peak memory.
//
// The output is a linear ProPhoto RGB F32 buffer (the same working space as
// the inputs).
//
// @note Alignment is translation-only: rotation/scale estimation is not
//       implemented, so sources must already share an orientation. Blending is
//       feather-weighted rather than multi-band, which can show seams on
//       exposures with strong gradients.

#include <memory>
#include <vector>

#include "engine/ImageBuffer.h"

namespace lrc {

/**
 * @brief Tunables for a panorama stitch.
 *
 * Fields are plain-ish defaults suitable for a typical horizontal landscape
 * sweep. `alignMaxDim` and `bandRows` trade quality-independent cost against
 * peak memory; `focalLengthMm` only matters in cylindrical mode.
 */
struct PanoramaOptions {
    float focalLengthMm = 35.0f;   // focal length estimate (mm), used for the cylindrical projection
    bool cylindrical = true;       // true = cylindrical projection; false = flat translation mosaic
    bool autoCrop = true;          // crop the result to the content bounding box
    int alignMaxDim = 512;         // longest edge of the alignment downscale (px)
    int bandRows = 64;             // blending row-band height (peak-memory control)
};

/**
 * @brief Stitches an ordered sequence of overlapping input images into one
 *        panorama.
 *
 * Semantics by input size:
 *   - empty input vector: returns nullptr,
 *   - single input: returns an F32 copy with identical dimensions,
 *   - two or more inputs: alignment + projection + feather blend.
 *
 * Inputs are treated as linear ProPhoto RGB in any of the U8/U16/F32 formats;
 * non-F32 inputs are converted internally. The output is linear ProPhoto RGB
 * F32 (the same working space as the inputs).
 *
 * @param inputs Ordered frames (left-to-right or right-to-left); members must
 *        be non-null and non-empty.
 * @param opts Behaviour knobs; defaults suit a typical landscape sweep.
 * @return The stitched panorama buffer, or nullptr for an empty input.
 *
 * @throws std::invalid_argument for null/empty members and std::runtime_error
 *         when an adjacent pair has no reliable overlap (NCC score below the
 *         internal threshold).
 */
std::shared_ptr<ImageBuffer> stitchPanorama(
    const std::vector<std::shared_ptr<ImageBuffer>>& inputs,
    const PanoramaOptions& opts = {});

} // namespace lrc
