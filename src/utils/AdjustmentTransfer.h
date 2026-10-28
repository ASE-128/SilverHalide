#pragma once

#include <cstdint>

namespace lrc {

struct Adjustments;

enum class AdjustmentCategory : uint32_t {
    None = 0,
    Basic = 1u << 0,
    WhiteBalance = 1u << 1,
    ToneCurve = 1u << 2,
    Color = 1u << 3,
    Detail = 1u << 4,
    Effects = 1u << 5,
    LensCorrections = 1u << 6,
    Crop = 1u << 7,
    Masks = 1u << 8,
    SpotRemoval = 1u << 9,
    AiDenoise = 1u << 10,
    All = (1u << 11) - 1,
};

constexpr AdjustmentCategory operator|(AdjustmentCategory lhs,
                                       AdjustmentCategory rhs) noexcept {
    return static_cast<AdjustmentCategory>(
        static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

constexpr AdjustmentCategory operator&(AdjustmentCategory lhs,
                                       AdjustmentCategory rhs) noexcept {
    return static_cast<AdjustmentCategory>(
        static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
}

constexpr AdjustmentCategory& operator|=(AdjustmentCategory& lhs,
                                         AdjustmentCategory rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

/**
 * @brief Copies only the explicitly selected adjustment categories.
 *
 * Photo-specific data such as mask geometry and spot-removal locations is
 * excluded unless its category is selected. `source` is never modified.
 */
void copyAdjustmentCategories(const Adjustments& source, Adjustments& target,
                              AdjustmentCategory categories);

} // namespace lrc
