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
    /** @brief Serializes an adjustment stack into a JSON value.
     *
     * @param adjustments Stack to encode. Only read during the call.
     * @return A new owned JSON value; discarding it is always a bug, hence
     *         [[nodiscard]].
     * @note Allocates the JSON tree and may throw std::bad_alloc.
     */
    [[nodiscard]] static json::Value toJson(const Adjustments& adjustments);

    /** @brief Reconstructs an adjustment stack from a serialized JSON value.
     *
     * Missing keys keep per-field defaults; unparseable or malformed values
     * fall back to the associated default rather than failing.
     *
     * @param value Encoded stack; only read during the call.
     * @return The reconstructed stack (never a failure signal, hence
     *         [[nodiscard]]).
     * @note Allocates the stack payload (strings / vectors / masks) and may
     *       throw std::bad_alloc.
     */
    [[nodiscard]] static Adjustments fromJson(const json::Value& value);

    /** @brief Writes a preset file to `path` (.lrp).
     *
     * The write is atomic (temp file + rename), so a crash never leaves a
     * truncated but apparently-valid preset on disk.
     *
     * @param path          Target preset file path.
     * @param adjustments   Stack to persist.
     * @return True on success; false on I/O failure. Ignoring the result
     *         would hide a lost preset write, hence [[nodiscard]].
     * @note Performs file I/O and allocates; it may throw std::bad_alloc but
     *       reports ordinary I/O failures through the return value.
     */
    [[nodiscard]] static bool savePreset(const std::filesystem::path& path,
                                         const Adjustments& adjustments);

    /** @brief Loads and reconstructs a preset from `path`.
     *
     * @param path Source preset file path.
     * @return The decoded adjustments, or nullopt if the file cannot be opened
     *         or parsed. Ignoring the result would silently drop a preset load,
     *         hence [[nodiscard]].
     * @note Performs file I/O and allocates; it may throw std::bad_alloc but
     *       reports ordinary I/O/parse failures as nullopt.
     */
    [[nodiscard]] static std::optional<Adjustments> loadPreset(
        const std::filesystem::path& path);
};

} // namespace lrc
