#include "engine/LensfunProfile.h"

#if LRC_ENABLE_LENSFUN
#include <lensfun/lensfun.h>
#include <algorithm>
#include <cmath>
#include <vector>
#endif

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#include <limits.h>
#endif

namespace lrc {

bool LensfunProfile::available() {
#if LRC_ENABLE_LENSFUN
    return true;
#else
    return false;
#endif
}

std::filesystem::path LensfunProfile::bundledDatabasePath() {
    // Locate the running executable's directory without Qt. The installed
    // layout places the Lensfun XML database at <appdir>/share/lensfun/
    // version_1, mirroring the conventional Lensfun install prefix.
    std::filesystem::path exeDir;
#if defined(_WIN32)
    wchar_t buf[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH)
        exeDir = std::filesystem::path(buf).parent_path();
#elif defined(__linux__)
    char buf[PATH_MAX] = {};
    const ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        exeDir = std::filesystem::path(buf).parent_path();
    }
#endif
    if (exeDir.empty())
        return {};
    const auto bundled = exeDir / "share" / "lensfun" / "version_1";
    std::error_code ec;
    if (std::filesystem::is_directory(bundled, ec))
        return bundled;
    return {};
}

std::vector<LensfunCorrection> LensfunProfile::find(
    const std::string& cameraMaker, const std::string& cameraModel,
    const std::string& lensMaker, const std::string& lensModel,
    const std::filesystem::path& databasePath) {
    std::vector<LensfunCorrection> result;
#if LRC_ENABLE_LENSFUN
    lfDatabase database;
    // Prefer an explicitly supplied path; otherwise fall back to the bundled
    // database shipped next to the executable, then to Lensfun's default
    // system-wide search.
    auto path = databasePath;
    if (path.empty())
        path = bundledDatabasePath();
    if (!path.empty())
        database.Load(path.string().c_str());
    else
        database.Load();
    const lfCamera** cameras = database.FindCameras(cameraMaker.c_str(),
                                                    cameraModel.c_str());
    if (!cameras)
        return result;
    for (size_t i = 0; cameras[i]; ++i) {
        const lfLens** lenses = database.FindLenses(cameras[i], lensMaker.c_str(),
                                lensModel.c_str());
        if (!lenses)
            continue;
        for (size_t j = 0; lenses[j]; ++j) {
            LensfunCorrection correction;
            correction.cameraMaker = cameraMaker;
            correction.cameraModel = cameraModel;
            correction.lensMaker = lensMaker;
            correction.lensModel = lensModel;
            correction.cropFactor = cameras[i]->CropFactor;
            correction.hasDistortion = true;
            correction.hasVignetting = true;
            correction.hasChromaticAberration = true;
            result.push_back(std::move(correction));
        }
        delete[] lenses;
    }
    delete[] cameras;
#else
    (void)cameraMaker;
    (void)cameraModel;
    (void)lensMaker;
    (void)lensModel;
    (void)databasePath;
#endif
    return result;
}

bool LensfunProfile::applyGeometry(const Adjustments& a, float* pixels,
                                   int width, int height,
                                   const std::filesystem::path& databasePath) {
#if LRC_ENABLE_LENSFUN
    if (!a.lensfunEnabled || !pixels || width <= 0 || height <= 0 ||
        a.lensfunFocalLengthMm <= 0.0f || a.lensfunAperture <= 0.0f)
        return false;
    lfDatabase database;
    // Prefer an explicitly supplied path; otherwise fall back to the bundled
    // database shipped next to the executable, then to Lensfun's default
    // system-wide search.
    auto path = databasePath;
    if (path.empty())
        path = bundledDatabasePath();
    if (!path.empty())
        database.Load(path.string().c_str());
    else
        database.Load();
    const lfCamera** cameras = database.FindCameras(
        a.lensfunCameraMaker.c_str(), a.lensfunCameraModel.c_str());
    if (!cameras)
        return false;
    const lfLens* lens = nullptr;
    for (size_t i = 0; cameras[i] && !lens; ++i) {
        const lfLens** lenses = database.FindLenses(
            cameras[i], a.lensfunLensMaker.c_str(), a.lensfunLensModel.c_str());
        if (lenses && lenses[0])
            lens = lenses[0];
        delete[] lenses;
    }
    if (!lens) {
        delete[] cameras;
        return false;
    }
    const float crop = a.lensfunCropFactor > 0.0f ? a.lensfunCropFactor : 1.0f;
    lfModifier modifier(lens, crop, width, height);
    const int flags = modifier.Initialize(
        lens, LF_PF_F32, a.lensfunFocalLengthMm, a.lensfunAperture, 1000.0f,
        0.0f, lens->Type, LF_MODIFY_DISTORTION, false);
    if ((flags & LF_MODIFY_DISTORTION) == 0) {
        delete[] cameras;
        return false;
    }
    std::vector<float> coords(static_cast<size_t>(width) * height * 2);
    if (!modifier.ApplyGeometryDistortion(0.0f, 0.0f, width, height,
                                          coords.data())) {
        delete[] cameras;
        return false;
    }
    std::vector<float> source(pixels, pixels +
                                       static_cast<size_t>(width) * height * 3);
    auto sample = [&](float x, float y, int channel) {
        x = std::clamp(x, 0.0f, static_cast<float>(width - 1));
        y = std::clamp(y, 0.0f, static_cast<float>(height - 1));
        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const int x1 = std::min(x0 + 1, width - 1);
        const int y1 = std::min(y0 + 1, height - 1);
        const float fx = x - x0;
        const float fy = y - y0;
        const auto at = [&](int px, int py) {
            return source[(static_cast<size_t>(py) * width + px) * 3 + channel];
        };
        return (at(x0, y0) * (1.0f - fx) + at(x1, y0) * fx) * (1.0f - fy) +
               (at(x0, y1) * (1.0f - fx) + at(x1, y1) * fx) * fy;
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t coord = (static_cast<size_t>(y) * width + x) * 2;
            const float sx = coords[coord];
            const float sy = coords[coord + 1];
            const size_t dst = (static_cast<size_t>(y) * width + x) * 3;
            pixels[dst] = sample(sx, sy, 0);
            pixels[dst + 1] = sample(sx, sy, 1);
            pixels[dst + 2] = sample(sx, sy, 2);
        }
    }
    delete[] cameras;
    return true;
#else
    (void)a; (void)pixels; (void)width; (void)height; (void)databasePath;
    return false;
#endif
}

} // namespace lrc
