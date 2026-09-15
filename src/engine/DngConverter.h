#pragma once

// Converts a raw file (ARW/DNG/NEF/...) into a *raw* DNG that preserves the
// sensor mosaic, CFA pattern, black/white levels, the camera->XYZ color matrix
// and the as-shot white balance. The output is NOT demosaiced (no render).
//
// Unlike RawDecoder this class never runs dcraw_process: it only calls
// LibRaw::open_file() + LibRaw::unpack() and copies the packed 16-bit mosaic
// straight into an uncompressed 16-bit CFA DNG.

#include <filesystem>
#include <string>

namespace lrc {

class DngConverter {
public:
    // Returns true on success; on failure *error holds a human-readable
    // message.
    static bool convert(const std::filesystem::path& inRaw,
                        const std::filesystem::path& outDng,
                        std::string* error);

    // Returns a non-clobbering ".dng" output path next to inRaw (same base
    // name). If "<base>.dng" already exists, appends "-2", "-3", ... like
    // Lightroom's "Import as DNG" does. Used by the import-as-DNG workflow.
    static std::filesystem::path suggestDngPath(
        const std::filesystem::path& inRaw);
};

} // namespace lrc
