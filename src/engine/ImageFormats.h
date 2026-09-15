#pragma once

// Central registry of image formats the app can import and how each is
// decoded. Files whose extension appears in rawExtensions() are decoded by
// libraw (RawDecoder); the remaining importable formats (JPEG/PNG/TIFF) are
// decoded by stb_image and converted into the linear ProPhoto working space.
// Keeping the lists here (engine layer, Qt-free) lets the catalog's
// FolderMonitor and the UI import dialogs share one source of truth.

#include <filesystem>
#include <string>
#include <vector>

namespace lrc {

// Case-insensitive lower-case extensions (leading dot) decoded by libraw.
const std::vector<std::string>& rawExtensions();

// All importable extensions: rawExtensions() plus JPEG/PNG/TIFF.
const std::vector<std::string>& importExtensions();

bool isRawPath(const std::filesystem::path& path);
bool isImportablePath(const std::filesystem::path& path);

} // namespace lrc
