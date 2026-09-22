#include "utils/PresetLibrary.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "utils/ProfileIO.h"

namespace lrc {

namespace {

// Characters that are illegal in Windows file names; rejected on every
// platform so a preset created on one OS can be read on another.
constexpr const char* kIllegalChars = "\\/:*?\"<>|";

bool isIllegalChar(char c) {
    return std::strchr(kIllegalChars, c) != nullptr ||
           static_cast<unsigned char>(c) < 0x20;  // control characters
}

} // namespace

PresetLibrary::PresetLibrary(std::filesystem::path rootDir)
    : rootDir_(std::move(rootDir)) {
    std::error_code ec;
    std::filesystem::create_directories(rootDir_, ec);
}

std::filesystem::path PresetLibrary::defaultRoot() {
    std::filesystem::path base;
#if defined(_WIN32)
    // Roaming %APPDATA% keeps presets with the user profile, matching the
    // "Silver Halide" directory convention used by ModelManager (which reads
    // %LOCALAPPDATA% for the model cache).
    char buf[32767] = {};
    size_t n = 0;
    if (getenv_s(&n, buf, "APPDATA") == 0 && n > 1) {
        base = buf;
    } else {
        const char* user = std::getenv("USERPROFILE");
        base = std::filesystem::path(user && *user ? user : ".") /
               "AppData" / "Roaming";
    }
    base /= "Silver Halide";
#else
    const char* home = std::getenv("HOME");
    if (!home || !*home)
        home = ".";
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg)
        base = xdg;
    else
        base = std::filesystem::path(home) / ".local" / "share";
    base /= "SilverHalide";
#endif
    base /= "presets";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base;
}

bool PresetLibrary::isValidName(const std::string& name) {
    // Trimmed copy; a name that is all whitespace is invalid.
    const auto begin = std::find_if_not(name.begin(), name.end(), [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    });
    const auto end = std::find_if_not(name.rbegin(), name.rend(), [](char c) {
                         return std::isspace(static_cast<unsigned char>(c)) != 0;
                     }).base();
    if (begin >= end || end - begin > 120)
        return false;
    // Leading dots would map to hidden files (or "." / ".."), breaking the
    // name <-> file mapping.
    if (*begin == '.')
        return false;
    return std::none_of(begin, end, isIllegalChar);
}

std::string PresetLibrary::sanitizeName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (isIllegalChar(c))
            out.push_back('_');
        else
            out.push_back(c);
    }
    // Trim surrounding whitespace (illegal whitespace-only remains "_"-free).
    const auto first = std::find_if_not(out.begin(), out.end(), [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    });
    const auto last = std::find_if_not(out.rbegin(), out.rend(), [](char c) {
                          return std::isspace(static_cast<unsigned char>(c)) != 0;
                      }).base();
    return (first < last) ? std::string(first, last) : std::string();
}

std::vector<std::string> PresetLibrary::listPresets() const {
    std::vector<std::string> names;
    std::error_code ec;
    if (!std::filesystem::is_directory(rootDir_, ec))
        return names;
    for (const auto& entry : std::filesystem::directory_iterator(rootDir_, ec)) {
        if (!entry.is_regular_file(ec))
            continue;
        const auto path = entry.path();
        if (path.extension() != ".lrp")
            continue;  // ignore .tmp leftovers and foreign files
        const std::string stem = path.stem().string();
        // Skip hidden files (e.g. ".foo.lrp").
        if (stem.empty() || stem.front() == '.')
            continue;
        names.push_back(stem);
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool PresetLibrary::savePreset(const std::string& name,
                               const Adjustments& adjustments) const {
    if (!isValidName(name))
        return false;
    std::error_code ec;
    std::filesystem::create_directories(rootDir_, ec);
    return ProfileIO::savePreset(pathFor(name), adjustments);
}

std::optional<Adjustments> PresetLibrary::loadPreset(
    const std::string& name) const {
    if (!isValidName(name))
        return std::nullopt;
    return ProfileIO::loadPreset(pathFor(name));
}

bool PresetLibrary::deletePreset(const std::string& name) const {
    if (!isValidName(name))
        return false;
    std::error_code ec;
    return std::filesystem::remove(pathFor(name), ec);
}

bool PresetLibrary::exportPreset(const std::string& name,
                                 const std::filesystem::path& destPath) const {
    if (!isValidName(name) || destPath.empty())
        return false;
    const auto adj = ProfileIO::loadPreset(pathFor(name));
    if (!adj)
        return false;
    // Reuse the atomic writer so an interrupted export never leaves a
    // truncated but apparently-valid destination file.
    return ProfileIO::savePreset(destPath, *adj);
}

bool PresetLibrary::exists(const std::string& name) const {
    if (!isValidName(name))
        return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(pathFor(name), ec);
}

std::filesystem::path PresetLibrary::pathFor(const std::string& name) const {
    return rootDir_ / (name + ".lrp");
}

} // namespace lrc
