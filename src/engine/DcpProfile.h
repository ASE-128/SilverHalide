#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lrc {

/**
 * @brief Data read from an Adobe DCP camera profile file.
 *
 * Holds the identifying fields, colour matrices and the hue/saturation/value
 * mapping used to render a camera-matching look. All fields are plain POD;
 * optional fields carry a parallel `hasXxx` flag so callers can distinguish
 * an absent tag from a zero value.
 */
struct DcpProfile {
    std::string name;
    std::string cameraModel;
    std::array<float, 9> colorMatrix1{};
    std::array<float, 9> colorMatrix2{};
    bool hasColorMatrix1 = false;
    bool hasColorMatrix2 = false;
    /** DNG LightSource codes of CalibrationIlluminant1/2 (e.g. 17 = Std A,
     *  21 = D65). [0]/[1] pair with the matrices and hue/sat maps of the
     *  same index; [2] is unused padding. */
    std::array<float, 3> calibrationIlluminant{};
    bool hasCalibrationIlluminant1 = false;
    bool hasCalibrationIlluminant2 = false;
    std::array<float, 256> toneCurve{};
    size_t toneCurveSize = 0;

    /** @brief Hue/sat grid divisions: hue, saturation, value (DCP tag
     *  50981). Hue wraps during lookup; saturation/value clamp. */
    std::array<uint32_t, 3> hueSatDims{0u, 0u, 0u};
    /** @brief ProfileHueSatMapData1/2 (DCP tags 50982/50983). Entries are
     *  triplets (hue shift in degrees, saturation scale, value scale) in
     *  nested value-outer/hue-middle/saturation-inner order; length is
     *  dims[0]*dims[1]*dims[2]*3. */
    std::vector<float> hueSatMap1;
    std::vector<float> hueSatMap2;
    bool hasHueSatMap1 = false;
    bool hasHueSatMap2 = false;
    /** @brief ProfileHueSatMapEncoding (DNG tag 51107): 0 = linear indexes,
     *  1 = value index sRGB-encoded. Absent tags default to 0 (linear). */
    int hueSatMapEncoding = 0;
};

/**
 * @brief Low-level reader for Adobe DCP camera profiles.
 *
 * Parses the DCP/TIFF tags needed to identify a profile and extract its colour
 * matrices. Pure reading: inputs are never mutated and no caching is done.
 */
class DcpProfileReader {
public:
    /**
     * @brief Reads the DCP/TIFF tags needed to identify a camera profile and
     *        its colour matrices.
     *
     * @param path Path to the DCP profile file.
     * @return The parsed profile, or std::nullopt when the file is invalid,
     *         truncated, or not a DCP.
     */
    static std::optional<DcpProfile> read(const std::filesystem::path& path);

    /**
     * @brief Tests whether a file is an Adobe DCP profile.
     *
     * DCP is a TIFF container with Adobe-specific tags. This only accepts a
     * file whose DCP/TIFF tags carry the Adobe DCP marker.
     *
     * @param path Path to the file.
     * @return true iff the file is a readable DCP container.
     */
    static bool isDcp(const std::filesystem::path& path);
};

} // namespace lrc
