#pragma once

// Serialization of adjustment stacks. Used by the catalog for persisting edits
// and by the UI for importing/exporting develop presets. The on-disk format is
// plain JSON, so presets remain human-readable and portable.

#include <filesystem>
#include <optional>

#include "engine/Adjustments.h"
#include "utils/Json.h"

namespace lrc {

class ProfileIO {
public:
    static json::Value toJson(const Adjustments& adjustments);
    static Adjustments fromJson(const json::Value& value);

    // Writes a preset file (extensions: .lrp). Returns false on I/O errors.
    static bool savePreset(const std::filesystem::path& path,
                           const Adjustments& adjustments);
    static std::optional<Adjustments> loadPreset(
        const std::filesystem::path& path);
};

} // namespace lrc
