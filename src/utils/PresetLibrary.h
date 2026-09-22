#pragma once

/**
 * @brief Manageable named-preset store for develop adjustment stacks.
 *
 * A library is a plain directory of `.lrp` files (one preset per file), by
 * default under `%APPDATA%/Silver Halide/presets` (Windows) or
 * `$XDG_DATA_HOME/SilverHalide/presets` (Unix). It wraps ProfileIO, so every
 * stored file is the same human-readable JSON used by the file-level
 * import/export actions.
 *
 * The class is Qt-free by design: it lives in src/utils and is exercised by
 * the headless test suite. All methods are const and stateless apart from the
 * root directory, and each preset maps to exactly one file, so concurrent
 * access is safe as long as callers do not race on the same preset name.
 */

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "engine/Adjustments.h"

namespace lrc {

class PresetLibrary {
public:
    /**
     * @brief Opens the library rooted at `rootDir`, creating it if missing.
     *
     * @param rootDir Library directory; defaults to defaultRoot().
     */
    explicit PresetLibrary(std::filesystem::path rootDir = defaultRoot());

    /**
     * @brief Returns the platform user-preset directory, creating it on demand.
     *
     * Windows: `%APPDATA%/Silver Halide/presets` (Roaming, so presets follow
     * the user across machines); other platforms:
     * `$XDG_DATA_HOME|~/.local/share/SilverHalide/presets`.
     */
    static std::filesystem::path defaultRoot();

    /**
     * @brief Checks whether `name` is a usable preset name.
     *
     * A valid name is non-empty after trimming, at most 120 characters, and
     * contains none of the characters illegal in file names, so the name <->
     * file mapping stays injective (one preset per `.lrp` file).
     */
    static bool isValidName(const std::string& name);

    /**
     * @brief Replaces file-name-illegal characters with '_' and trims.
     *
     * Used by the UI to propose a safe file stem from free-text input. The
     * result may be empty when the input is empty or all-illegal.
     */
    static std::string sanitizeName(const std::string& name);

    /**
     * @brief Lists the names of all stored presets, sorted lexicographically.
     *
     * Non-`.lrp` files (e.g. leftover `.tmp` files from an interrupted atomic
     * write) and hidden dot-files are ignored.
     *
     * @return Preset names (file stems without the `.lrp` extension).
     */
    std::vector<std::string> listPresets() const;

    /**
     * @brief Saves `adjustments` under `name`, overwriting any existing preset
     *        with the same name (a save is an update, so names stay unique).
     *
     * @param name        Target preset name (must satisfy isValidName).
     * @param adjustments Stack to persist.
     * @return True on success; false for an invalid name or an I/O failure.
     */
    bool savePreset(const std::string& name,
                    const Adjustments& adjustments) const;

    /**
     * @brief Loads the preset stored under `name`.
     *
     * @return The decoded adjustments, or nullopt if the preset does not exist
     *         or the file is corrupted. Never throws for malformed content.
     */
    std::optional<Adjustments> loadPreset(const std::string& name) const;

    /**
     * @brief Deletes the preset stored under `name`.
     *
     * @return True when a preset was removed; false if the name is invalid or
     *         no such preset exists.
     */
    bool deletePreset(const std::string& name) const;

    /**
     * @brief Copies the preset `name` to `destPath` (any location / extension).
     *
     * The copy is written atomically via ProfileIO::savePreset so the
     * destination never appears half-written.
     *
     * @return True on success; false if the name is invalid or the source
     *         preset is missing/corrupted or the destination cannot be written.
     */
    bool exportPreset(const std::string& name,
                      const std::filesystem::path& destPath) const;

    /** @brief True when a preset with this name is currently stored. */
    bool exists(const std::string& name) const;

    /** @brief Absolute `.lrp` path that the preset `name` maps to (no I/O). */
    std::filesystem::path pathFor(const std::string& name) const;

private:
    std::filesystem::path rootDir_;
};

} // namespace lrc
