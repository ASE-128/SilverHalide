#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace lrc {

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

class DcpProfileReader {
public:
    // Reads the DCP/TIFF tags needed to identify a camera profile and its
    // color matrices. Invalid or truncated files return std::nullopt.
    static std::optional<DcpProfile> read(const std::filesystem::path& path);

    // DCP is a TIFF container with Adobe-specific tags. This only accepts a
    // file whose DNGPrivateData contains the Adobe DCP marker.
    static bool isDcp(const std::filesystem::path& path);
};

} // namespace lrc
