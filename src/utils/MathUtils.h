#pragma once

// Small math helpers shared by the engine and catalog layers.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>

namespace lrc {

constexpr double kPi = 3.14159265358979323846;

/** @brief Clamps `value` to [lo, hi]. */
template <typename T>
constexpr T clamp(T value, T lo, T hi) {
    return std::clamp(value, lo, hi);
}

/** @brief Linear interpolation between a and b for t in [0, 1].
 *
 * @param a Value at t = 0.
 * @param b Value at t = 1.
 * @param t Interpolation factor; values outside [0, 1] extrapolate.
 * @return a + (b - a) * t, with the same operation order as before.
 *
 * @note constexpr and noexcept: pure scalar arithmetic with no allocation and
 *       no shared state, so it is safe from any thread and usable in constant
 *       expressions.
 */
constexpr double lerp(double a, double b, double t) noexcept {
    return a + (b - a) * t;
}

/** @brief Hermite smoothstep over [edge0, edge1].
 *
 * Outputs 0 before edge0, 1 after edge1, and a smooth S-curve in between.
 *
 * @param edge0 Lower edge of the transition; must differ from edge1.
 * @param edge1 Upper edge of the transition.
 * @param x     Input value.
 * @return 0 for x <= edge0, 1 for x >= edge1, otherwise t * t * (3 - 2t)
 *         with t = clamp((x - edge0) / (edge1 - edge0), 0, 1).
 *
 * @note constexpr and noexcept: pure scalar arithmetic, thread-safe; the
 *       expression order is unchanged from the runtime-only version.
 */
constexpr double smoothstep(double edge0, double edge1, double x) noexcept {
    const double t =
        clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/** @brief FNV-1a style 64-bit hash mixing (a single step).
 *
 * @param seed  Accumulated hash so far.
 * @param value Value to mix into the hash.
 * @return The mixed hash, using the exact same constants and shift sequence
 *         as before so existing cache keys stay stable.
 *
 * @note constexpr and noexcept: unsigned integer arithmetic only, thread-safe.
 */
constexpr size_t hashCombine(size_t seed, size_t value) noexcept {
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    return seed;
}

/** @brief Converts an exposure delta in stops to a linear multiplier.
 *
 * @param ev Exposure delta in stops; 0 maps to 1.0.
 * @return 2^ev.
 *
 * @note noexcept only, not constexpr: std::pow() is not usable in constant
 *       expressions under C++20 (it becomes constexpr in C++23). The
 *       implementation deliberately stays std::pow so results remain
 *       bit-identical to the previous version.
 */
inline double stopsToGain(double ev) noexcept { return std::pow(2.0, ev); }

/** @brief Maps a -100..100 slider value into a multiplier around 1.0.
 *
 * @param value Slider position in [-100, 100]; 0 maps to 1.0.
 * @param range Exposure range in stops spanned by the full slider travel.
 * @return 2^(value / 100 * range).
 *
 * @note noexcept only, not constexpr: std::pow() is not usable in constant
 *       expressions under C++20. The expression order is unchanged.
 */
inline double sliderToScale(double value, double range) noexcept {
    return std::pow(2.0, value / 100.0 * range);
}

/** @brief The eight colour ranges (red/orange/yellow/green/aqua/blue/purple/
 *         magenta) and their centre hue angles in degrees.
 *
 * Shared by the pipeline's HSL handling and the targeted-adjustment colour
 * sampler.
 */
constexpr float kColorRangeCenters[8] = {
    0.0f, 30.0f, 60.0f, 120.0f, 180.0f, 240.0f, 290.0f, 330.0f};

/** @brief Index (0..7) of the colour range whose centre hue is nearest the
 *         given hue, wrapping around 360°.
 *
 * @note noexcept: pure float arithmetic over a fixed lookup table, no
 *       allocation and no shared state.
 */
inline int nearestColorRange(float hueDegrees) noexcept {
    int best = 0;
    float bestDist = 1e9f;
    for (int i = 0; i < 8; ++i) {
        float d = std::abs(hueDegrees - kColorRangeCenters[i]);
        if (d > 180.0f)
            d = 360.0f - d;
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

/** @brief True if the two doubles are bitwise-identical (used by cache keys).
 *
 * @note noexcept: std::bit_cast of two scalars, no allocation.
 */
inline bool bitEqual(double a, double b) noexcept {
    return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b);
}

// --- UTF-8 <-> filesystem::path -------------------------------------------
// On MSVC, std::filesystem::path(const std::string&) interprets the input in
// the process ANSI codepage, which corrupts UTF-8 paths (e.g. Chinese
// filenames). These helpers go through the UTF-16 path type explicitly.

/** @brief Converts a UTF-8 string to a filesystem path token-safe on MSVC. */
inline std::filesystem::path utf8ToPath(const std::string& utf8) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

/** @brief Converts a filesystem path back to a UTF-8 string. */
inline std::string pathToUtf8(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// --- LibRaw path opening ---------------------------------------------------
/**
 * @brief Opens a RAW file with LibRaw using the encoding LibRaw expects.
 *
 * LibRaw's narrow open_file() interprets its argument in the process ANSI code
 * page, so a path outside that code page is opened incorrectly or not at all.
 * Windows builds of LibRaw also provide a wchar_t overload, which is used
 * here; on POSIX the narrow form is already UTF-8 and stays as it is.
 *
 * @param raw  A LibRaw (or compatible) instance. Templated so this header does
 *             not have to include libraw.
 * @param path File to open.
 * @return The LibRaw status code, where 0 means LIBRAW_SUCCESS.
 */
template <typename RawType>
int openRawFile(RawType& raw, const std::filesystem::path& path) {
#if defined(_WIN32)
    return raw.open_file(path.wstring().c_str());
#else
    return raw.open_file(path.string().c_str());
#endif
}

} // namespace lrc
