#pragma once

// Full-resolution render + encoding. JPEG export runs the pipeline at native
// resolution (never the cached preview) and writes 8-bit sRGB output.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/Adjustments.h"
#include "engine/ImageBuffer.h"

namespace lrc {

class ColorManager;
class Pipeline;

class ExportEngine {
public:
    ExportEngine(std::shared_ptr<const ColorManager> colorManager,
                 Pipeline& pipeline);

    // Renders `linear` through `adjustments` at full resolution and writes a
    // 8-bit sRGB JPEG. quality is 0..100. Returns false on I/O or encode
    // errors (message is reported via lastError()).
    bool exportJpeg(const std::filesystem::path& outPath, uint64_t imageId,
                    const ImageBuffer& linear,
                    const Adjustments& adjustments, int quality);

    // Renders and writes an 8-bit sRGB PNG (lossless).
    bool exportPng(const std::filesystem::path& outPath, uint64_t imageId,
                   const ImageBuffer& linear,
                   const Adjustments& adjustments);

    // Renders and writes an sRGB TIFF at 8- or 16-bit precision.
    // bitDepth must be 8 or 16.
    bool exportTiff(const std::filesystem::path& outPath, uint64_t imageId,
                    const ImageBuffer& linear,
                    const Adjustments& adjustments, int bitDepth);

    // Renders and writes a *linear* DNG (16-bit, demosaiced RGB with DNG tags)
    // from the linear working-space buffer. Round-trips through RawDecoder.
    bool exportDng(const std::filesystem::path& outPath, uint64_t imageId,
                   const ImageBuffer& linear,
                   const Adjustments& adjustments);

    // S16: unified encode-and-write entry point. Dispatches on spec.format and
    // quality/maxDim. Returns false on any render / I/O / encode error. This is
    // the single call site that callers (e.g. PreviewWorker::batchExport in
    // S17-18) should use when they need a generic export path.
    struct ExportSpec {
        enum class Format { Jpeg, Png, Tiff8, Tiff16, Dng };
        Format format = Format::Jpeg;
        int quality = 90;   // JPEG only
        int maxDim = 0;     // >0 resizes the longest edge before encoding
    };
    bool encodeAndWrite(const std::filesystem::path& outPath, uint64_t imageId,
                        const ImageBuffer& linear,
                        const Adjustments& adjustments,
                        const ExportSpec& spec);

    const std::string& lastError() const { return lastError_; }

    // Writes a U8 RGB buffer as a binary P6 PPM (used by the thumbnail cache
    // as a Qt-readable, dependency-free thumbnail format).
    static bool writePPM(const std::filesystem::path& outPath,
                         const ImageBuffer& rgbU8);

private:
    std::shared_ptr<const ColorManager> cm_;
    Pipeline& pipeline_;
    std::string lastError_;

    // S16: parallel strip-wise F32 -> U16 quantization. Slices the source F32
    // image into row-strips of `rowsPerStrip` rows and quantizes each strip
    // independently on the thread pool, writing directly into the matching
    // region of `u16`. `convert` optionally maps linear ProPhoto -> display
    // sRGB before the 0..1 -> 0..65535 clamp. Bit-for-bit identical to the
    // old single-threaded loop (same per-pixel math, just partitioned).
    static void quantizeToU16Parallel(
        const ImageBuffer& f32, ImageBuffer& u16, int rowsPerStrip,
        const std::function<void(const float*, float*, size_t)>& convert);
};

} // namespace lrc
