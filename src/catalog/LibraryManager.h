#pragma once

// Managed-library import policy: where imported photos go and whether they are
// copied or moved. Mirrors Lightroom's import dialog (Add / Copy / Move).
// Pure C++ — no Qt.

#include <filesystem>
#include <optional>
#include <string>

namespace lrc {

enum class ImportMode {
    Add,  // reference in place, leave the file untouched on disk
    Copy, // copy into the managed library, keep the source file
    Move, // move into the managed library (the source file is removed)
};

/**
 * @brief Pure std::filesystem helpers for Lightroom-style managed-library
 *        imports.
 *
 * Files that already live under `libraryRoot` are returned unchanged so a
 * re-import never copies a library file onto itself.
 */
class LibraryManager {
public:
    /** @brief Destination path for `src`:
     *         <root>/<yyyy>/<yyyy-mm-dd>/<name>, where the date comes from the
     *         file's modification time (the closest proxy for the capture date
     *         without EXIF). When the target already exists a numeric suffix
     *         (-2, -3, ...) is appended before the extension.
     *
     *  @return `src` itself when it already sits under the library root. */
    static std::filesystem::path suggestDestPath(
        const std::filesystem::path& src,
        const std::filesystem::path& libraryRoot);

    /** @brief Places `src` into the library per `mode`.
     *
     *  Add returns `src` unchanged; Copy copies to the suggested destination;
     *  Move copies then removes the source. The destination directories are
     *  created as needed.
     *
     *  @param libraryRoot Managed-library root; files under it are untouched.
     *  @param mode        Add / Copy / Move.
     *  @param error       Optional out-param populated with a message on
     *                     failure.
     *  @return The cataloged path on success, or nullopt with `error` set
     *          (when non-null) on failure. */
    static std::optional<std::filesystem::path> importFile(
        const std::filesystem::path& src,
        const std::filesystem::path& libraryRoot, ImportMode mode,
        std::string* error = nullptr);

private:
    LibraryManager() = delete;
};

} // namespace lrc
