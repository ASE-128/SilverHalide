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
    // S16: unified export parameters carried by every per-format method. The
    // P1-2 ICC preset and the P1-4 watermark are optional; leaving them at their
    // defaults keeps the historical byte-identical output.
    struct ExportSpec {
        enum class Format { Jpeg, Png, Tiff8, Tiff16, Dng };
        Format format = Format::Jpeg;
        int quality = 90;   // JPEG only
        int maxDim = 0;     // >0 resizes the longest edge before encoding

        // P1-2: ICC preset to embed in the exported file. None keeps the
        // historical byte-identical output (no profile). Applied to the
        // colour-managed raster formats (JPEG / PNG / TIFF-8 / TIFF-16).
        enum class IccChoice : int { None = 0, SRGB, AdobeRGB, DisplayP3 };
        IccChoice icc = IccChoice::None;
        std::filesystem::path iccCustomPath;  // optional user-supplied profile

        // P1-4: optional watermark overlay. `rgba` holds straight-alpha RGBA
        // pixels of size overlayW x overlayH. When `enabled`, the overlay is
        // composited over the U8 display output (JPEG / PNG / TIFF-8) at
        // `scale` (overlay long edge / output long edge), at `position` with
        // `opacity`. Not applied to linear archival formats (TIFF-16 / DNG).
        struct Watermark {
            bool enabled = false;
            std::vector<uint8_t> rgba;   // w*h*4, straight alpha
            int overlayW = 0;
            int overlayH = 0;
            int position = 4;   // 0..3 corners (TL,TR,BL,BR), 4 = centre
            float opacity = 1.0f; // 0..1
            float scale = 0.2f;   // overlay long edge relative to output
        };
        Watermark watermark;
    };

    ExportEngine(std::shared_ptr<const ColorManager> colorManager,
                 Pipeline& pipeline);

    // Renders `linear` through `adjustments` at full resolution and writes a
    // 8-bit sRGB JPEG. quality is 0..100. Returns false on I/O or encode
    // errors (message is reported via lastError()). `spec` carries the
    // optional ICC profile and watermark for the P1-2 / P1-4 export options.
    bool exportJpeg(const std::filesystem::path& outPath, uint64_t imageId,
                    const ImageBuffer& linear,
                    const Adjustments& adjustments, int quality,
                    const ExportSpec& spec = ExportSpec{});

    // Renders and writes an 8-bit sRGB PNG (lossless). `spec` carries the
    // optional ICC profile and watermark (P1-2 / P1-4).
    bool exportPng(const std::filesystem::path& outPath, uint64_t imageId,
                   const ImageBuffer& linear,
                   const Adjustments& adjustments,
                   const ExportSpec& spec = ExportSpec{});

    // Renders and writes an sRGB TIFF at 8- or 16-bit precision.
    // bitDepth must be 8 or 16. `spec` carries the optional ICC profile for the
    // 8-bit path and the watermark (applied to the U8 output only).
    bool exportTiff(const std::filesystem::path& outPath, uint64_t imageId,
                    const ImageBuffer& linear,
                    const Adjustments& adjustments, int bitDepth,
                    const ExportSpec& spec = ExportSpec{});

    // Renders and writes a *linear* DNG (16-bit, demosaiced RGB with DNG tags)
    // from the linear working-space buffer. Round-trips through RawDecoder.
    // `spec` carries the optional ICC profile for DNG embedding; watermark
    // is intentionally skipped for linear archival formats.
    bool exportDng(const std::filesystem::path& outPath, uint64_t imageId,
                   const ImageBuffer& linear,
                   const Adjustments& adjustments,
                   const ExportSpec& spec = ExportSpec{});

    // S16: unified encode-and-write entry point. Dispatches on spec.format and
    // quality/maxDim. Returns false on any render / I/O / encode error. This is
    // the single call site that callers (e.g. PreviewWorker::batchExport in
    // S17-18) should use when they need a generic export path.
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
