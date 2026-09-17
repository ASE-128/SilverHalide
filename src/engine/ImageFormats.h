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

/**
 * @brief Lower-case extensions (with leading dot) decoded by libraw.
 * @return Reference to the shared list; do not modify.
 */
const std::vector<std::string>& rawExtensions();

/**
 * @brief All importable extensions: rawExtensions() plus JPEG/PNG/TIFF.
 * @return Reference to the shared list; do not modify.
 */
const std::vector<std::string>& importExtensions();

/**
 * @brief Tests whether a path's extension is a raw format decoded by libraw.
 */
bool isRawPath(const std::filesystem::path& path);

/**
 * @brief Tests whether a path's extension is importable by the app.
 */
bool isImportablePath(const std::filesystem::path& path);

} // namespace lrc
