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

/**
 * @brief Converts a raw file into an uncompressed, raw (non-demosaiced) DNG.
 *
 * The output preserves the sensor mosaic, CFA pattern, black/white levels, the
 * camera->XYZ colour matrix and the as-shot white balance. Oldest supported
 * output is an uncompressed 16-bit CFA DNG. Unlike RawDecoder this class never
 * runs dcraw_process: it only calls LibRaw::open_file() + LibRaw::unpack() and
 * copies the packed 16-bit mosaic straight into the DNG.
 */
class DngConverter {
public:
    /**
     * @brief Converts `inRaw` to a raw DNG at `outDng`.
     *
     * @param inRaw Source raw file (ARW/DNG/NEF/...).
     * @param outDng Destination DNG path (created/overwritten).
     * @param error Optional pointer to a human-readable message on failure.
     * @return true on success; false on failure (with *error set when non-null).
     */
    static bool convert(const std::filesystem::path& inRaw,
                        const std::filesystem::path& outDng,
                        std::string* error);

    /**
     * @brief Returns a non-clobbering ".dng" output path next to inRaw (same
     *        base name).
     *
     * If "<base>.dng" already exists, appends "-2", "-3", ... like Lightroom's
     * "Import as DNG" does. Used by the import-as-DNG workflow.
     *
     * @param inRaw Source raw file path.
     * @return A suggested destination path that does not yet exist (falls back
     *         to the base name after 1000 existing copies).
     */
    static std::filesystem::path suggestDngPath(
        const std::filesystem::path& inRaw);
};

} // namespace lrc
