#pragma once

// Exposure-bracketed HDR merge (tripod-style). The input frames are linear
// ProPhoto RGB buffers (typically from the same raw file bracketed in
// different exposures). The merge:
//
//   1. estimates a relative exposure offset (in stops) for each frame when the
//      caller does not supply one (largest frame is the reference),
//   2. exposure-normalizes every frame to the reference exposure,
//   3. blends with luminance-based well-exposedness weights so each output
//      pixel is dominated by the frame(s) whose mid-tones captured it best,
//   4. optionally de-ghosts moving content with a robust (median-biased) blend.
//
// Pure C++, Qt-free, unit tested.

#include <cstdint>
#include <string>
#include <vector>

#include "engine/ImageBuffer.h"

namespace lrc {

class HdrMerge {
public:
    /**
     * @brief Result of an HDR merge.
     *
     * `linear` is null when the merge failed; `ok()` is the success test.
     * `evOffsets` holds the exposure offsets actually used (one per frame, in
     * stops, relative to the reference frame).
     */
    struct Result {
        ImageBufferPtr linear;            // merged linear ProPhoto F32 buffer
        std::vector<double> evOffsets;    // offsets used (stops, frame vs ref)
        std::string error;                // non-empty on failure
        /** @brief True when the merge produced a non-empty linear buffer.
         *
         * @note Never throws: it only inspects the two members above.
         */
        bool ok() const noexcept { return linear && !linear->empty(); }
    };

    /**
     * @brief Merges exposure-bracketed frames into one linear buffer.
     *
     * Merging is strictly per-pixel and streams in row bands, so peak memory
     * is bounded by @p bandRows regardless of frame count.
     *
     * @param frames Input linear ProPhoto frames (any of U8/U16/F32), all the
     *        same dimensions and at least two.
     * @param evOffsets Per-frame exposure offset in stops relative to frame 0.
     *        If empty or a different size, offsets are estimated from the
     *        luminance of each frame.
     * @param deghost Enables the robust (median-biased) blend for hand-held /
     *        moving-subject shots.
     * @param bandRows Row-band height used for streaming the merge (memory
     *        bound). The result is bit-identical regardless of its value.
     * @return A Result whose `ok()` is false (with `error` set) on invalid
     *         input, otherwise the merged linear ProPhoto F32 buffer.
     *
     * @note Not noexcept: the output buffers and the per-band scratch
     *       allocate and may throw std::bad_alloc. No [[nodiscard]] is
     *       attached because Result is a value type that some callers may
     *       construct/discard in tests.
     */
    static Result merge(const std::vector<ImageBufferPtr>& frames,
                        const std::vector<double>& evOffsets, bool deghost,
                        int bandRows = 128);

    /**
     * @brief Estimates the exposure offset of `other` relative to `ref`.
     *
     * A positive result means `other` is brighter. Computed as the median
     * log-luminance ratio on a downscaled luma image.
     *
     * @param ref Reference frame.
     * @param other Frame whose offset is estimated.
     * @return Estimated exposure offset in stops; 0 when no usable pixels.
     *
     * @note Discarding the estimate is always a bug, so the return value is
     *       [[nodiscard]]. Not noexcept: the downscaled luma scratch buffers
     *       allocate and may throw std::bad_alloc.
     */
    [[nodiscard]] static double estimateEv(const ImageBuffer& ref,
                                           const ImageBuffer& other);

private:
    // Downscales to a small grayscale luma image (maxDim longest side).
    static std::vector<float> lumaSmall(const ImageBuffer& buf, int maxDim,
                                        int& w, int& h);
};

} // namespace lrc
