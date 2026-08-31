#include "engine/ExportEngine.h"

#include <algorithm>
#include <functional>
#include <vector>

#include "engine/ColorManager.h"
#include "engine/ImageIO.h"
#include "engine/Pipeline.h"
#include "utils/Parallel.h"

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

void ExportEngine::quantizeToU16Parallel(
    const ImageBuffer& f32, ImageBuffer& u16, int rowsPerStrip,
    const std::function<void(const float*, float*, size_t)>& convert) {
    // S16: quantize F32 -> U16 with row-strip parallelism. Each strip is an
    // independent range of rows; chunks never overlap so no locks are needed.
    // The per-pixel math is identical to the old single-threaded loop (same
    // clamp * 65535.0f cast), so the output is bit-for-bit identical.
    const int w = f32.width();
    const int h = f32.height();
    if (w <= 0 || h <= 0)
        return;
    const int strip = (rowsPerStrip > 0) ? rowsPerStrip : 64;

    // If a colour-space conversion is needed, it operates on a contiguous
    // range of pixels; each strip gets its own temp buffer so the pool
    // threads never share writable state.
    auto perStrip = [&](size_t rowBegin, size_t rowEnd) {
        const size_t stripRows = rowEnd - rowBegin;
        const size_t stripPixels = stripRows * static_cast<size_t>(w);
        const size_t stripFloats = stripPixels * 3;
        const float* src = reinterpret_cast<const float*>(f32.data().data()) +
                           rowBegin * static_cast<size_t>(w) * 3;
        auto* dst = reinterpret_cast<uint16_t*>(u16.data().data()) +
                    rowBegin * static_cast<size_t>(w) * 3;

        std::vector<float> buf;
        const float* data = src;
        if (convert) {
            buf.resize(stripFloats);
            convert(src, buf.data(), stripPixels);
            data = buf.data();
        }
        for (size_t i = 0; i < stripFloats; ++i)
            dst[i] = static_cast<uint16_t>(
                std::clamp(data[i], 0.0f, 1.0f) * 65535.0f);
    };

    const size_t nStrips = static_cast<size_t>((h + strip - 1) / strip);
    const size_t grain = std::max<size_t>(1, nStrips / 8);
    lrc::parallel::parallelFor(nStrips, grain, [&](size_t s, size_t) {
        const int rowBegin = static_cast<int>(s * strip);
        const int rowEnd = std::min(rowBegin + strip, h);
        perStrip(static_cast<size_t>(rowBegin), static_cast<size_t>(rowEnd));
    });
}

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
        // S16: strip-parallel quantization (replaces the old serial loop).
        quantizeToU16Parallel(
            *lin, *u16, 64, [&](const float* s, float* d, size_t n) {
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
    // S16: strip-parallel quantization, no colour conversion (linear DNG).
    quantizeToU16Parallel(*lin, *u16, 64, nullptr);
    if (!ImageIO::writeDng(outPath, *u16)) {
        lastError_ = "cannot write output file";
        return false;
    }
    return true;
}

bool ExportEngine::encodeAndWrite(const std::filesystem::path& outPath,
                                  uint64_t imageId, const ImageBuffer& linear,
                                  const Adjustments& adjustments,
                                  const ExportSpec& spec) {
    // S17-18: optional maxDim downscale. Applied in linear F32 space before
    // the per-format dispatch so every format takes the same path. The resize
    // is area-average (never upscales), so a maxDim >= native longest edge is
    // a no-op and the output stays at full resolution. When a resize happens
    // we recurse with maxDim cleared so the dispatch runs exactly once.
    if (spec.maxDim > 0) {
        const int longest = std::max(linear.width(), linear.height());
        if (longest > spec.maxDim) {
            const double s =
                static_cast<double>(spec.maxDim) / longest;
            const int nw = std::max(1, static_cast<int>(linear.width() * s));
            const int nh = std::max(1, static_cast<int>(linear.height() * s));
            ImageBufferPtr scaled = (linear.format() == PixelFormat::F32)
                ? linear.resize(nw, nh)
                : linear.resizeToF32(nw, nh);
            if (!scaled || scaled->empty()) {
                lastError_ = "resize failed";
                return false;
            }
            ExportSpec scaledSpec = spec;
            scaledSpec.maxDim = 0;  // avoid infinite recursion
            return encodeAndWrite(outPath, imageId, *scaled, adjustments,
                                  scaledSpec);
        }
    }
    // S16: single dispatch entry point for all export formats.
    switch (spec.format) {
        case ExportSpec::Format::Jpeg:
            return exportJpeg(outPath, imageId, linear, adjustments,
                             spec.quality);
        case ExportSpec::Format::Png:
            return exportPng(outPath, imageId, linear, adjustments);
        case ExportSpec::Format::Tiff8:
            return exportTiff(outPath, imageId, linear, adjustments, 8);
        case ExportSpec::Format::Tiff16:
            return exportTiff(outPath, imageId, linear, adjustments, 16);
        case ExportSpec::Format::Dng:
            return exportDng(outPath, imageId, linear, adjustments);
    }
    lastError_ = "unknown export format";
    return false;
}

bool ExportEngine::writePPM(const std::filesystem::path& outPath,
                            const ImageBuffer& rgbU8) {
    return ImageIO::writePPM(outPath, rgbU8);
}

} // namespace lrc
