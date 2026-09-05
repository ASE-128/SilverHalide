#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "engine/Adjustments.h"

namespace lrc {

struct LensfunCorrection {
    std::string cameraMaker;
    std::string cameraModel;
    std::string lensMaker;
    std::string lensModel;
    float cropFactor = 1.0f;
    bool hasDistortion = false;
    bool hasVignetting = false;
    bool hasChromaticAberration = false;
};

class LensfunProfile {
public:
    // Returns whether this build was compiled with the vcpkg Lensfun library.
    static bool available();

    // Resolves the bundled Lensfun database shipped alongside the application.
    // Returns the path `<exedir>/share/lensfun/version_1` when that directory
    // exists (installed layout), otherwise an empty path so callers fall back
    // to Lensfun's built-in default search (system-wide databases). Engine is
    // Qt-free, so the executable directory is derived from the OS module path.
    static std::filesystem::path bundledDatabasePath();

    // Looks up a camera/lens pair in Lensfun's installed XML database. An
    // empty result means the database or the requested profile was unavailable.
    static std::vector<LensfunCorrection> find(
        const std::string& cameraMaker, const std::string& cameraModel,
        const std::string& lensMaker, const std::string& lensModel,
        const std::filesystem::path& databasePath = {});

    // Applies Lensfun's geometric correction in place to an interleaved F32
    // RGB buffer. Returns false when the profile or capture parameters are
    // unavailable; callers must then leave the buffer unchanged.
    static bool applyGeometry(const Adjustments& adjustments, float* pixels,
                              int width, int height,
                              const std::filesystem::path& databasePath = {});
};

} // namespace lrc
