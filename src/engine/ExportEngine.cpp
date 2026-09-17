#include "engine/ExportEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <vector>

#include "engine/ColorManager.h"
#include "engine/ImageIO.h"
#include "engine/Pipeline.h"
#include "utils/Parallel.h"

namespace lrc {

namespace {

// ---------------------------------------------------------------------------
// ICC profile resolution + watermark compositing helpers
// ---------------------------------------------------------------------------

// Resolves the ICC profile bytes to embed for a spec. Priority: a custom user
// profile path, then a named preset. Empty when neither applies (None/absent).
std::vector<uint8_t> resolveIccBytes(const ExportEngine::ExportSpec& spec) {
    if (!spec.iccCustomPath.empty()) {
        // Read the file raw — the profile is passed through verbatim.
        std::ifstream in(spec.iccCustomPath, std::ios::binary);
        if (!in)
            return {};
        in.seekg(0, std::ios::end);
        const std::streamsize sz = in.tellg();
        if (sz <= 0 || sz > 1 << 24)
            return {};
        in.seekg(0, std::ios::beg);
        std::vector<uint8_t> bytes(static_cast<size_t>(sz));
        in.read(reinterpret_cast<char*>(bytes.data()), sz);
        if (!in.good() && !in.eof())
            return {};
        return bytes;
    }
    switch (spec.icc) {
        case ExportEngine::ExportSpec::IccChoice::SRGB:
            return ColorManager::exportProfileBytes(
                ColorManager::ExportColorProfile::SRGB);
        case ExportEngine::ExportSpec::IccChoice::AdobeRGB:
            return ColorManager::exportProfileBytes(
                ColorManager::ExportColorProfile::AdobeRGB);
        case ExportEngine::ExportSpec::IccChoice::DisplayP3:
            return ColorManager::exportProfileBytes(
                ColorManager::ExportColorProfile::DisplayP3);
        default:
            return {};
    }
}

// Scales the overlay (nearest-neighbour — deterministic, reference-clean) to a
// width/height and writes straight-alpha RGBA into `rw x rh` output.
std::vector<uint8_t> scaleOverlay(const std::vector<uint8_t>& rgba, int sw,
                                  int sh, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return {};
    std::vector<uint8_t> out(static_cast<size_t>(dw) * dh * 4);
    for (int y = 0; y < dh; ++y) {
        const int sy = std::min(sh - 1, (y * sh) / dh);
        for (int x = 0; x < dw; ++x) {
            const int sx = std::min(sw - 1, (x * sw) / dw);
            const size_t si = (static_cast<size_t>(sy) * sw + sx) * 4;
            const size_t di = (static_cast<size_t>(y) * dw + x) * 4;
            out[di] = rgba[si];
            out[di + 1] = rgba[si + 1];
            out[di + 2] = rgba[si + 2];
            out[di + 3] = rgba[si + 3];
        }
    }
    return out;
}

// Composites the (already scaled) straight-alpha overlay at (ox,oy) over the
// U8 output. dst = bg*(1-a) + wm*a, where a = opacity * overlayA/255.
void compositeOverlay(uint8_t* dst, int outW, int outH,
                      const std::vector<uint8_t>& wm, int ww, int wh,
                      int ox, int oy, float opacity) {
    for (int y = 0; y < wh; ++y) {
        const int dy = oy + y;
        if (dy < 0 || dy >= outH)
            continue;
        for (int x = 0; x < ww; ++x) {
            const int dx = ox + x;
            if (dx < 0 || dx >= outW)
                continue;
            const size_t si = (static_cast<size_t>(y) * ww + x) * 4;
            const float a = opacity * (wm[si + 3] / 255.0f);
            if (a <= 0.0f)
                continue;
            const size_t di = (static_cast<size_t>(dy) * outW + dx) * 3;
            for (int c = 0; c < 3; ++c) {
                const float bg = dst[di + c] / 255.0f;
                const float fg = wm[si + c] / 255.0f;
                const float v = bg * (1.0f - a) + fg * a;
                dst[di + c] = static_cast<uint8_t>(
                    std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
    }
}

// Applies a colour-space preset watermark or returns the buffer untouched.
// (This is a compile guard to keep the engine free of Qt; see applyWatermark.)

ImageBufferPtr applyWatermark(const ImageBufferPtr& rgb,
                              const ExportEngine::ExportSpec& spec) {
    const auto& wm = spec.watermark;
    if (!wm.enabled || wm.rgba.empty() || wm.overlayW <= 0 || wm.overlayH <= 0 ||
        wm.opacity <= 0.0f)
        return rgb;
    const int outW = rgb->width();
    const int outH = rgb->height();
    if (outW <= 0 || outH <= 0)
        return rgb;

    // Desired overlay size: `scale` is the ratio of overlay long edge to output
    // long edge. Preserve the overlay's aspect ratio.
    const int longest = std::max(outW, outH);
    const int wl = wm.overlayW, hl = wm.overlayH;
    double wScale = static_cast<double>(longest) * wm.scale / std::max(1, std::max(wl, hl));
    if (wScale <= 0.0)
        return rgb;
    int ww = std::max(1, static_cast<int>(wl * wScale + 0.5));
    int wh = std::max(1, static_cast<int>(hl * wScale + 0.5));
    if (ww > outW) ww = outW;
    if (ww < 1) ww = 1;
    if (wh > outH) wh = outH;

    const auto scaled = scaleOverlay(wm.rgba, wl, hl, ww, wh);
    if (scaled.empty())
        return rgb;

    // Anchor.
    const int margin = std::max(1, longest / 50);
    int ox = 0, oy = 0;
    switch (wm.position) {
        case 0: ox = margin;            oy = margin;            break;  // TL
        case 1: ox = outW - ww - margin; oy = margin;           break;  // TR
        case 2: ox = margin;            oy = outH - wh - margin; break; // BL
        case 3: ox = outW - ww - margin; oy = outH - wh - margin; break; // BR
        default: ox = (outW - ww) / 2; oy = (outH - wh) / 2;    break;  // centre
    }

    // Composite onto a copy so the cached/reference buffer is never mutated.
    auto out = std::make_shared<ImageBuffer>(*rgb);
    uint8_t* dst = out->data().data();
    compositeOverlay(dst, outW, outH, scaled, ww, wh, ox, oy, wm.opacity);
    return out;
}

} // namespace

ExportEngine::ExportEngine(std::shared_ptr<const ColorManager> colorManager,
                           Pipeline& pipeline)
    : cm_(std::move(colorManager)), pipeline_(pipeline) {}

bool ExportEngine::exportJpeg(const std::filesystem::path& outPath,
                              uint64_t imageId, const ImageBuffer& linear,
                              const Adjustments& adjustments, int quality,
                              const ExportSpec& spec) {
    lastError_.clear();
    ImageBufferPtr rgb = pipeline_.renderFullDisplay(imageId, linear, adjustments);
    if (!rgb || rgb->format() != PixelFormat::U8 || rgb->empty()) {
        lastError_ = "render failed";
        return false;
    }
    rgb = applyWatermark(rgb, spec);
    const auto icc = resolveIccBytes(spec);
    // Convert display sRGB pixels into the chosen ICC space so the
    // embedded profile and the pixel data describe the same colour space.
    rgb = cm_->sRGBEncodedToProfile(*rgb, icc);
    if (!ImageIO::writeJpeg(outPath, *rgb, quality, icc)) {
        lastError_ = "cannot write output file";
        return false;
    }
    return true;
}

void ExportEngine::quantizeToU16Parallel(
    const ImageBuffer& f32, ImageBuffer& u16, int rowsPerStrip,
    const std::function<void(const float*, float*, size_t)>& convert) {
    // Quantize F32 -> U16 with row-strip parallelism. Each strip is an
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
                             const Adjustments& adjustments,
                             const ExportSpec& spec) {
    lastError_.clear();
    ImageBufferPtr rgb = pipeline_.renderFullDisplay(imageId, linear, adjustments);
    if (!rgb || rgb->format() != PixelFormat::U8 || rgb->empty()) {
        lastError_ = "render failed";
        return false;
    }
    rgb = applyWatermark(rgb, spec);
    const auto icc = resolveIccBytes(spec);
    // Real sRGB -> target ICC pixel conversion (see sRGBEncodedToProfile).
    rgb = cm_->sRGBEncodedToProfile(*rgb, icc);
    if (!ImageIO::writePng(outPath, *rgb, icc)) {
        lastError_ = "cannot write output file";
        return false;
    }
    return true;
}

bool ExportEngine::exportTiff(const std::filesystem::path& outPath,
                              uint64_t imageId, const ImageBuffer& linear,
                              const Adjustments& adjustments, int bitDepth,
                              const ExportSpec& spec) {
    lastError_.clear();
    // The ICC profile is embedded on both the 8- and 16-bit raster paths; the
    // watermark only on the U8 (display) path.
    if (bitDepth == 8) {
        ImageBufferPtr rgb = pipeline_.renderFullDisplay(imageId, linear, adjustments);
        if (!rgb || rgb->format() != PixelFormat::U8 || rgb->empty()) {
            lastError_ = "render failed";
            return false;
        }
        rgb = applyWatermark(rgb, spec);
        const auto icc8 = resolveIccBytes(spec);
        // Mirror JPEG/PNG — convert the display sRGB pixels into the chosen
        // ICC space before embedding the same profile, so pixels and tag agree.
        rgb = cm_->sRGBEncodedToProfile(*rgb, icc8);
        if (!ImageIO::writeTiff(outPath, *rgb, icc8)) {
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
        // Strip-parallel quantization (replaces the old serial loop).
        quantizeToU16Parallel(
            *lin, *u16, 64, [&](const float* s, float* d, size_t n) {
                cm_->linearProPhotoToSRGB(s, d, n);
            });
        const auto icc16 = resolveIccBytes(spec);
        // The pixels above are sRGB-encoded; convert them into the chosen
        // ICC space so the embedded profile and the data describe the same colour
        // space. When icc16 is empty (None/sRGB default) this is a no-op clone.
        const auto conv = cm_->sRGBEncodedToProfile(*u16, icc16);
        const ImageBuffer& out = conv ? *conv : *u16;
        if (!ImageIO::writeTiff(outPath, out, icc16)) {
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
                             const Adjustments& adjustments,
                             const ExportSpec& spec) {
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
    // Strip-parallel quantization, no colour conversion (linear DNG).
    quantizeToU16Parallel(*lin, *u16, 64, nullptr);
    // Linear DNG carries linear ProPhoto working-space data that is NOT
    // converted into any display space, so embedding a display ICC (sRGB /
    // Adobe RGB / Display P3) would mislabel the colour space. The profile is
    // intentionally omitted; the dialog disables the
    // ICC (and watermark) choices for DNG so nothing is silently dropped.
    if (!ImageIO::writeDng(outPath, *u16, {})) {
        lastError_ = "cannot write output file";
        return false;
    }
    return true;
}

bool ExportEngine::encodeAndWrite(const std::filesystem::path& outPath,
                                  uint64_t imageId, const ImageBuffer& linear,
                                  const Adjustments& adjustments,
                                  const ExportSpec& spec) {
    // Optional maxDim downscale. Applied in linear F32 space before
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
    // Single dispatch entry point for all export formats.
    switch (spec.format) {
        case ExportSpec::Format::Jpeg:
            return exportJpeg(outPath, imageId, linear, adjustments,
                             spec.quality, spec);
        case ExportSpec::Format::Png:
            return exportPng(outPath, imageId, linear, adjustments, spec);
        case ExportSpec::Format::Tiff8:
            return exportTiff(outPath, imageId, linear, adjustments, 8, spec);
        case ExportSpec::Format::Tiff16:
            return exportTiff(outPath, imageId, linear, adjustments, 16, spec);
        case ExportSpec::Format::Dng:
            return exportDng(outPath, imageId, linear, adjustments, spec);
    }
    lastError_ = "unknown export format";
    return false;
}

bool ExportEngine::writePPM(const std::filesystem::path& outPath,
                            const ImageBuffer& rgbU8) {
    return ImageIO::writePPM(outPath, rgbU8);
}

} // namespace lrc
