#pragma once

// Full-resolution render + encoding. JPEG export runs the pipeline at native
// resolution (never the cached preview) and writes 8-bit sRGB output.

#include <cstdint>
#include <filesystem>
#include <memory>

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

    const std::string& lastError() const { return lastError_; }

    // Writes a U8 RGB buffer as a binary P6 PPM (used by the thumbnail cache
    // as a Qt-readable, dependency-free thumbnail format).
    static bool writePPM(const std::filesystem::path& outPath,
                         const ImageBuffer& rgbU8);

private:
    std::shared_ptr<const ColorManager> cm_;
    Pipeline& pipeline_;
    std::string lastError_;
};

} // namespace lrc
