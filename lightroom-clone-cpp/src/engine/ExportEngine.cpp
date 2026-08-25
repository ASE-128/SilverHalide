#include "engine/ExportEngine.h"

#include <algorithm>
#include <functional>
#include <vector>

#include "engine/ColorManager.h"
#include "engine/ImageIO.h"
#include "engine/Pipeline.h"

namespace lrc {

ExportEngine::ExportEngine(std::shared_ptr<const ColorManager> colorManager,
                           Pipeline& pipeline)
    : cm_(std::move(colorManager)), pipeline_(pipeline) {}

bool ExportEngine::exportJpeg(const std::filesystem::path& outPath,
                              uint64_t imageId, const ImageBuffer& linear,
                              const Adjustments& adjustments, int quality) {
    lastError_.clear();
    ImageBufferPtr rgb = pipeline_.renderFullDisplay(imageId, linear, adjustments);
    if (!rgb || rgb->format() != PixelFormat::U8 || rgb->empty()) {
        lastError_ = "render failed";
        return false;
    }
    if (!ImageIO::writeJpeg(outPath, *rgb, quality)) {
        lastError_ = "cannot write output file";
        return false;
    }
    return true;
}

namespace {

// Quantizes an F32 RGB buffer into a U16 RGB buffer. `convert` optionally maps
// the values first (used to encode linear ProPhoto -> display sRGB).
void quantizeToU16(const ImageBuffer& f32, ImageBuffer& u16,
                   const std::function<void(const float*, float*, size_t)>&
                       convert) {
    const size_t pixelCount =
        static_cast<size_t>(f32.width()) * f32.height();
    const size_t total = pixelCount * 3;
    const float* src = reinterpret_cast<const float*>(f32.data().data());
    std::vector<float> buf;
    const float* data = src;
    if (convert) {
        buf.resize(total);
        convert(src, buf.data(), pixelCount);
        data = buf.data();
    }
    auto* dst = reinterpret_cast<uint16_t*>(u16.data().data());
    for (size_t i = 0; i < total; ++i)
        dst[i] = static_cast<uint16_t>(std::clamp(data[i], 0.0f, 1.0f) * 65535.0f);
}

} // namespace

bool ExportEngine::exportPng(const std::filesystem::path& outPath,
                             uint64_t imageId, const ImageBuffer& linear,
                             const Adjustments& adjustments) {
    lastError_.clear();
    ImageBufferPtr rgb = pipeline_.renderFullDisplay(imageId, linear, adjustments);
    if (!rgb || rgb->format() != PixelFormat::U8 || rgb->empty()) {
        lastError_ = "render failed";
        return false;
    }
    if (!ImageIO::writePng(outPath, *rgb)) {
        lastError_ = "cannot write output file";
        return false;
    }
    return true;
}

bool ExportEngine::exportTiff(const std::filesystem::path& outPath,
                              uint64_t imageId, const ImageBuffer& linear,
                              const Adjustments& adjustments, int bitDepth) {
    lastError_.clear();
    if (bitDepth == 8) {
        ImageBufferPtr rgb = pipeline_.renderFullDisplay(imageId, linear, adjustments);
        if (!rgb || rgb->format() != PixelFormat::U8 || rgb->empty()) {
            lastError_ = "render failed";
            return false;
        }
        if (!ImageIO::writeTiff(outPath, *rgb)) {
            lastError_ = "cannot write output file";
            return false;
        }
        return true;
    }
    if (bitDepth == 16) {
        ImageBufferPtr lin =
            pipeline_.renderLinearFull(imageId, linear, adjustments);
        if (!lin || lin->format() != PixelFormat::F32 || lin->empty()) {
            lastError_ = "render failed";
            return false;
        }
        auto u16 = ImageBuffer::create(lin->width(), lin->height(),
                                       PixelFormat::U16);
        quantizeToU16(
            *lin, *u16, [&](const float* s, float* d, size_t n) {
                cm_->linearProPhotoToSRGB(s, d, n);
            });
        if (!ImageIO::writeTiff(outPath, *u16)) {
            lastError_ = "cannot write output file";
            return false;
        }
        return true;
    }
    lastError_ = "TIFF bit depth must be 8 or 16";
    return false;
}

bool ExportEngine::exportDng(const std::filesystem::path& outPath,
                             uint64_t imageId, const ImageBuffer& linear,
                             const Adjustments& adjustments) {
    lastError_.clear();
    // Linear DNG stores the working-space data unchanged (no sRGB conversion),
    // preserving the widest tonal range for later re-editing.
    ImageBufferPtr lin =
        pipeline_.renderLinearFull(imageId, linear, adjustments);
    if (!lin || lin->format() != PixelFormat::F32 || lin->empty()) {
        lastError_ = "render failed";
        return false;
    }
    auto u16 = ImageBuffer::create(lin->width(), lin->height(),
                                   PixelFormat::U16);
    quantizeToU16(*lin, *u16, nullptr);
    if (!ImageIO::writeDng(outPath, *u16)) {
        lastError_ = "cannot write output file";
        return false;
    }
    return true;
}

bool ExportEngine::writePPM(const std::filesystem::path& outPath,
                            const ImageBuffer& rgbU8) {
    return ImageIO::writePPM(outPath, rgbU8);
}

} // namespace lrc
