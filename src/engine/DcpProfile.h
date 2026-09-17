#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace lrc {

/**
 * @brief Data read from an Adobe DCP camera profile file.
 *
 * Holds the identifying fields and colour matrices used to classify a camera
 * profile. All fields are plain POD; optional fields carry a parallel
 * `hasXxx` flag so callers can distinguish an absent tag from a zero value.
 */
struct DcpProfile {
    std::string name;
    std::string cameraModel;
    std::array<float, 9> colorMatrix1{};
    std::array<float, 9> colorMatrix2{};
    bool hasColorMatrix1 = false;
    bool hasColorMatrix2 = false;
    std::array<float, 3> calibrationIlluminant{};
    bool hasCalibrationIlluminant1 = false;
    bool hasCalibrationIlluminant2 = false;
    std::array<float, 256> toneCurve{};
    size_t toneCurveSize = 0;
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
