#pragma once

#include <filesystem>
#include <optional>

#include "engine/Adjustments.h"
#include "utils/Json.h"

namespace lrc {

/**
 * @brief Serializes and deserializes the adjustment stack.
 *
 * Used by the catalog for persisting edits and by the UI for importing /
 * exporting develop presets. The on-disk format is plain JSON, so presets stay
 * human-readable and portable.
 */
class ProfileIO {
public:
    /** @brief Serializes an adjustment stack into a JSON value. */
    static json::Value toJson(const Adjustments& adjustments);

    /** @brief Reconstructs an adjustment stack from a serialized JSON value.
     *
     * Missing keys keep per-field defaults; unparseable or malformed values
     * fall back to the associated default rather than failing.
     */
    static Adjustments fromJson(const json::Value& value);

    /** @brief Writes a preset file to `path` (.lrp).
     *
     * The write is atomic (temp file + rename), so a crash never leaves a
     * truncated but apparently-valid preset on disk.
     *
     * @param path          Target preset file path.
     * @param adjustments   Stack to persist.
     * @return True on success; false on I/O failure.
     */
    static bool savePreset(const std::filesystem::path& path,
                           const Adjustments& adjustments);

    /** @brief Loads and reconstructs a preset from `path`.
     *
     * @param path Source preset file path.
     * @return The decoded adjustments, or nullopt if the file cannot be opened
     *         or parsed.
     */
    static std::optional<Adjustments> loadPreset(
        const std::filesystem::path& path);
};

} // namespace lrc
