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

/** @brief Linear interpolation between a and b for t in [0, 1]. */
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }

/** @brief Hermite smoothstep over [edge0, edge1].
 *
 * Outputs 0 before edge0, 1 after edge1, and a smooth S-curve in between.
 */
inline double smoothstep(double edge0, double edge1, double x) {
    const double t =
        clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/** @brief FNV-1a style 64-bit hash mixing (a single step). */
inline size_t hashCombine(size_t seed, size_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    return seed;
}

/** @brief Converts an exposure delta in stops to a linear multiplier. */
inline double stopsToGain(double ev) { return std::pow(2.0, ev); }

/** @brief Maps a -100..100 slider value into a multiplier around 1.0. */
inline double sliderToScale(double value, double range) {
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
 *         given hue, wrapping around 360°. */
inline int nearestColorRange(float hueDegrees) {
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

/** @brief True if the two doubles are bitwise-identical (used by cache keys). */
inline bool bitEqual(double a, double b) {
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
