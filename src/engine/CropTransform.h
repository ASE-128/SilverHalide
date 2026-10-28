#pragma once

#include "engine/Adjustments.h"
#include "engine/ImageBuffer.h"

namespace lrc {

/**
 * @brief Applies the stack's normalized crop and straighten transform.
 *
 * The input is never mutated. Invalid crop geometry falls back to an
 * unmodified copy; out-of-image samples created by rotation are black.
 */
[[nodiscard]] ImageBufferPtr applyCropAndStraighten(
    const ImageBuffer& source, const CropRect& crop, double angleDegrees);

[[nodiscard]] bool hasCropTransform(const CropRect& crop,
                                    double angleDegrees) noexcept;

} // namespace lrc
