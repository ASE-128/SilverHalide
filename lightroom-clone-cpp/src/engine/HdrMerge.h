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
    struct Result {
        ImageBufferPtr linear;            // merged linear ProPhoto F32 buffer
        std::vector<double> evOffsets;    // offsets used (stops, frame vs ref)
        std::string error;                // non-empty on failure
        bool ok() const { return linear && !linear->empty(); }
    };

    // Merges bracketed frames. If `evOffsets` is empty (or a different size),
    // offsets are estimated from the luminance of each frame. `deghost` enables
    // the robust blend for hand-held / moving-subject shots. `bandRows` bounds
    // the row-band height used for streaming the merge (memory bound); the
    // result is bit-identical regardless of its value.
    static Result merge(const std::vector<ImageBufferPtr>& frames,
                        const std::vector<double>& evOffsets, bool deghost,
                        int bandRows = 128);

    // Estimates the exposure offset in stops of `other` relative to `ref`
    // (positive means `other` is brighter) from the median log-luminance ratio
    // on a downscaled luminance image.
    static double estimateEv(const ImageBuffer& ref, const ImageBuffer& other);

private:
    // Downscales to a small grayscale luma image (maxDim longest side).
    static std::vector<float> lumaSmall(const ImageBuffer& buf, int maxDim,
                                        int& w, int& h);
};

} // namespace lrc
