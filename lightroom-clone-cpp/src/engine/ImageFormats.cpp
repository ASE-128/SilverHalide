#include "engine/ImageFormats.h"

#include <algorithm>
#include <cctype>

namespace lrc {

namespace {

std::string lowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    });
    return ext;
}

} // namespace

const std::vector<std::string>& rawExtensions() {
    // Sony uses four RAW extensions (ARW/SR2/SRF/SR3); the rest cover the
    // common camera manufacturers libraw understands.
    static const std::vector<std::string> kExts = {
        ".arw", ".sr2", ".srf", ".sr3", ".raw", ".mrw", ".dng", ".nef",
        ".cr2", ".cr3", ".raf", ".orf", ".rw2", ".pef", ".srw"};
    return kExts;
}

const std::vector<std::string>& importExtensions() {
    // Raw formats plus the formats the app can export (JPEG/PNG/TIFF), so
    // anything produced here can be imported back and re-edited. DNG is in
    // rawExtensions(), so it is covered by the raw half of the list.
    static const std::vector<std::string> kExts = [] {
        std::vector<std::string> exts = rawExtensions();
        exts.insert(exts.end(),
                    {".jpg", ".jpeg", ".png", ".tif", ".tiff"});
        return exts;
    }();
    return kExts;
}

bool isRawPath(const std::filesystem::path& path) {
    const std::string ext = lowerExtension(path);
    for (const std::string& e : rawExtensions())
        if (ext == e)
            return true;
    return false;
}

bool isImportablePath(const std::filesystem::path& path) {
    const std::string ext = lowerExtension(path);
    for (const std::string& e : importExtensions())
        if (ext == e)
            return true;
    return false;
}

} // namespace lrc
