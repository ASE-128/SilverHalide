#include "ui/PreviewWorker.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include <QDebug>
#include <QElapsedTimer>

#include "ui/Logging.h"

#include "app/Constants.h"
#include "catalog/CatalogDatabase.h"
#include "catalog/ThumbnailCache.h"
#include "engine/ColorManager.h"
#include "engine/DngConverter.h"
#include "engine/ExportEngine.h"
#include "engine/HdrMerge.h"
#include "engine/ImageIO.h"
#include "engine/Pipeline.h"
#include "engine/RawDecoder.h"

namespace {

std::filesystem::path toStdPath(const QString& path) {
    return std::filesystem::path(path.toStdWString());
}

// Monotonic millisecond stamp for the [PERF1A] 1:1 entry instrumentation.
// QElapsedTimer::elapsed() is const and safe to call from several threads on one
// instance, and the clock starts once at first use, so the deltas stay meaningful
// across the queued UI -> worker hop (and a wall-clock jump cannot skew them).
qint64 perfStampMs() {
    static QElapsedTimer timer = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return timer.elapsed();
}

// Copies a queued ExportWatermark into the engine ExportSpec (Qt vector ->
// std vector; the pixel bytes are copied verbatim).
lrc::ExportEngine::ExportSpec::Watermark toEngineWatermark(
    const ExportWatermark& wm) {
    lrc::ExportEngine::ExportSpec::Watermark out;
    out.enabled = wm.enabled;
    out.overlayW = wm.overlayW;
    out.overlayH = wm.overlayH;
    out.position = wm.position;
    out.opacity = wm.opacity;
    out.scale = wm.scale;
    if (!wm.rgba.isEmpty()) {
        out.rgba.reserve(static_cast<size_t>(wm.rgba.size()));
        for (qint32 v : wm.rgba)
            out.rgba.push_back(static_cast<uint8_t>(v));
    }
    return out;
}

// Applies the ICC + watermark export options to an ExportSpec.
void applyExportOptions(lrc::ExportEngine::ExportSpec& spec, int icc,
                        const ExportWatermark& wm) {
    spec.icc = static_cast<lrc::ExportEngine::ExportSpec::IccChoice>(icc);
    spec.watermark = toEngineWatermark(wm);
}

// Normalised 0..1 colour component -> 8-bit, the encoding the engine's soft
// proofing params and gamut alarm codes use.
uint8_t unitToU8(float v) {
    return static_cast<uint8_t>(
        std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// Zero-copy ImageBufferPtr -> QImage: the QImage borrows the buffer's U8 pixel
// data and a cleanup-info holder keeps the buffer alive for the QImage's whole
// lifetime (same pattern as MainWindow::toQImage). The buffer is immutable once
// rendered/cached, so sharing pixels across threads is safe.
QImage toQImage(const lrc::ImageBufferPtr& buffer) {
    if (!buffer || buffer->format() != lrc::PixelFormat::U8 ||
        buffer->empty())
        return {};
    auto* holder = new lrc::ImageBufferPtr(buffer);  // keeps the buffer alive
    QImage img(buffer->data().data(), buffer->width(), buffer->height(),
               buffer->width() * 3, QImage::Format_RGB888,
               [](void* ptr) { delete static_cast<lrc::ImageBufferPtr*>(ptr); },
               holder);
    if (img.isNull()) {  // construction failed; free the holder
        delete holder;
        return {};
    }
    return img;
}

} // namespace

PreviewWorker::PreviewWorker(lrc::RawDecoder& decoder,
                             lrc::Pipeline& pipeline,
                             lrc::ThumbnailCache& fullResCache,
                             lrc::CatalogDatabase& catalog,
                             std::shared_ptr<lrc::ColorManager> colorManager,
                             QObject* parent)
    : QObject(parent),
      decoder_(decoder),
      pipeline_(pipeline),
      fullResCache_(fullResCache),
      catalog_(catalog),
      colorManager_(std::move(colorManager)) {
    exportEngine_ = std::make_unique<lrc::ExportEngine>(colorManager_, pipeline_);
}

PreviewWorker::~PreviewWorker() = default;

void PreviewWorker::setDcpProfile(const QString& path) {
    // Store the parsed profile on the worker thread. An empty path (or a file
    // that no longer reads as a DCP) clears the selection, restoring libraw's
    // built-in camera matrix on the next decode.
    if (path.isEmpty()) {
        dcp_.reset();
        return;
    }
    auto profile = lrc::DcpProfileReader::read(toStdPath(path));
    if (!profile)
        dcp_.reset();
    else
        dcp_ = std::move(profile);
}

void PreviewWorker::loadImage(qint64 imageId, const QString& filePath,
                              qint64 generation,
                              lrc::AdjustmentsPtr adjustmentsPtr) {
    if (!adjustmentsPtr)
        return;  // defensive: the UI always publishes a stack.
    const lrc::Adjustments& adjustments = *adjustmentsPtr;
    if (abort_.load())
        return;  // abort requested.
    latestGeneration_ = generation;
    // Release full-res state when switching to a different image.
    if (imageId != fullResImageId_)
        clearFullRes();

    // Prefer the import-time standard preview (2048px linear ProPhoto F32) when
    // rendering the Fit/<=1:1 view at viewport resolution. The v2 blob carries
    // the native full-res dimensions, so imageMeta is emitted here too -- the
    // UI gets the true photo size without a RAW decode. For v1 blobs
    // (native==0) fall back to a 2x estimate so the spatial-kernel sigma scaling
    // stays conservative (slightly larger kernels are safe; too small would
    // under-sharpen).
    if (!dcp_ && !viewportSize_.isEmpty() && catalog_.hasStandardPreview(imageId)) {
        if (auto sp = catalog_.getStandardPreviewWithMeta(imageId)) {
            if (sp->buf && !sp->buf->empty()) {
                linear_ = sp->buf;
                loadedImageId_ = imageId;
                // The standard-preview fast path uses import-time linear data
                // and normally never reaches a RAW decode, so the camera
                // metadata signal that drives DCP/Lensfun auto-matching would
                // never fire. Read the metadata from the source file (no raw
                // demosaic) and emit it so the UI stays in sync even when the
                // preview is served from the catalog cache.
                if (auto meta = decoder_.readMeta(toStdPath(filePath))) {
                    if (generation == latestGeneration_)
                        emit cameraMeta(
                            imageId, QString::fromStdString(meta->make),
                            QString::fromStdString(meta->model),
                            QString::fromStdString(meta->lensMaker),
                            QString::fromStdString(meta->lensModel),
                            meta->focalLengthMm, meta->aperture,
                            meta->cropFactor);
                }
                if (sp->nativeWidth > 0 && sp->nativeHeight > 0) {
                    fullLongEdge_ = std::max(sp->nativeWidth,
                                             sp->nativeHeight);
                    if (generation == latestGeneration_)
                        emit imageMeta(imageId, sp->nativeWidth,
                                       sp->nativeHeight);
                } else {
                    // v1 blob: no native dimensions. Use 2x the preview's
                    // long edge as a conservative estimate (the standard
                    // preview was built from a 2x-oversampled decode).
                    fullLongEdge_ = std::max(sp->buf->width(),
                                             sp->buf->height()) * 2;
                }
                lrc::ImageBufferPtr linearDisplay;
                auto out = pipeline_.renderPreview(
                    *linear_, adjustments,
                    lrc::Size{viewportSize_.width(), viewportSize_.height()},
                    fullLongEdge_, static_cast<uint64_t>(imageId),
                    &linearDisplay);
                if (abort_.load())
                    return;  // abort during render.
                emitPreviewWithHistogram(imageId, generation, linearDisplay,
                                         out);
                return;
            }
        }
    }

    // Fallback: decode the raw at preview resolution (historical behaviour).
    lrc::DecodeParams params;
    params.halfSize = true;
    params.maxDim = lrc::app::kPreviewMaxDim;

    auto decoded = decoder_.decode(toStdPath(filePath), params);
    if (abort_.load())
        return;  // abort during decode.
    if (!decoded) {
        // Generation guard: a stale decode failure must not surface as a
        // "load failed" error once a newer request has superseded it.
        if (generation == latestGeneration_)
            emit loadFailed(filePath);
        return;
    }
    linear_ = decoded->linear;
    loadedImageId_ = imageId;
    fullLongEdge_ = std::max(decoded->meta.nativeWidth,
                             decoded->meta.nativeHeight);
    if (generation == latestGeneration_)
        emit imageMeta(imageId, decoded->meta.nativeWidth,
                       decoded->meta.nativeHeight);
    if (generation == latestGeneration_)
        emit cameraMeta(imageId, QString::fromStdString(decoded->meta.make),
                QString::fromStdString(decoded->meta.model),
                QString::fromStdString(decoded->meta.lensMaker),
                QString::fromStdString(decoded->meta.lensModel),
                decoded->meta.focalLengthMm, decoded->meta.aperture,
                decoded->meta.cropFactor);

    lrc::ImageBufferPtr out;
    if (!viewportSize_.isEmpty()) {
        lrc::ImageBufferPtr linearDisplay;
        out = pipeline_.renderPreview(
            *linear_, adjustments,
            lrc::Size{viewportSize_.width(), viewportSize_.height()},
            fullLongEdge_, static_cast<uint64_t>(imageId),
            &linearDisplay);
        if (abort_.load())
            return;  // abort during render.
        emitPreviewWithHistogram(imageId, generation, linearDisplay, out);
    } else {
        out = pipeline_.render(imageId, *linear_, adjustments, 0);
        if (abort_.load())
            return;  // abort during render.
        emitPreviewWithHistogram(imageId, generation, nullptr, out);
    }
}

void PreviewWorker::setPreviewTargetSize(const QSize& size) {
    // Called on the worker thread via a queued connection from the UI thread,
    // so no lock is needed. Merely stores the size; it is consumed by the next
    // loadImage/adjustPreview. Resize-triggered re-renders are issued by
    // MainWindow.
    viewportSize_ = size;
}

void PreviewWorker::adjustPreview(qint64 imageId, qint64 generation,
                                  lrc::AdjustmentsPtr adjustmentsPtr) {
    // A2 metric: worker-thread wall clock from the request being dequeued to the
    // refined previewReady emission. Coarse frames are deliberately excluded.
    QElapsedTimer refineTimer;
    refineTimer.start();
    if (!adjustmentsPtr)
        return;  // defensive: the UI always publishes a stack.
    const lrc::Adjustments& adjustments = *adjustmentsPtr;
    if (abort_.load())
        return;  // abort requested; skip stale queued re-renders.
    if (imageId != loadedImageId_ || !linear_)
        return;
    latestGeneration_ = generation;
    lrc::ImageBufferPtr out;
    if (!viewportSize_.isEmpty()) {
        // Render at the viewport's logical resolution through the incremental
        // preview pipeline (per-stage cache reuses unchanged stages).
        lrc::ImageBufferPtr linearDisplay;
        out = pipeline_.renderPreview(
            *linear_, adjustments,
            lrc::Size{viewportSize_.width(), viewportSize_.height()},
            fullLongEdge_, static_cast<uint64_t>(imageId),
            &linearDisplay);
        if (abort_.load())
            return;  // abort during render.
        emitPreviewWithHistogram(imageId, generation, linearDisplay, out,
                                 refineTimer.nsecsElapsed() / 1.0e6);
    } else {
        out = pipeline_.render(imageId, *linear_, adjustments, 0);
        if (abort_.load())
            return;  // abort during render.
        emitPreviewWithHistogram(imageId, generation, nullptr, out,
                                 refineTimer.nsecsElapsed() / 1.0e6);
    }
}

void PreviewWorker::adjustPreviewCoarse(qint64 imageId, qint64 generation,
                                        lrc::AdjustmentsPtr adjustmentsPtr) {
    if (!adjustmentsPtr)
        return;  // defensive: the UI always publishes a stack.
    const lrc::Adjustments& adjustments = *adjustmentsPtr;
    if (abort_.load())
        return;  // abort requested; skip stale queued re-renders.
    if (imageId != loadedImageId_ || !linear_)
        return;
    latestGeneration_ = generation;
    auto out = pipeline_.render(imageId, *linear_, adjustments,
                                lrc::app::kCoarseMaxDim);
    if (abort_.load())
        return;  // abort during render.
    // Soft proofing is part of the displayed colour, so the coarse drag frame has
    // to carry it too: otherwise the image visibly jumps colour on release.
    // Proofing needs the linear stage, which render() does not hand back, so it
    // is rendered once at the coarse size (renderLinearBounded skips the sRGB
    // conversion and the LRU, so this is one bounded pass).
    if (proofArmed()) {
        if (auto lin = pipeline_.renderLinearBounded(*linear_, adjustments,
                                                    lrc::app::kCoarseMaxDim)) {
            if (auto proofed = proofLinear(lin))
                out = proofed;
        }
        if (abort_.load())
            return;  // abort during the proof.
    }
    if (generation == latestGeneration_)
        emit previewReadyCoarse(imageId, out);
}

// --- Soft proofing (视图 → 软打样…") ------------------------------------------
//
// The proof leg is colourimetric: it needs the *adjusted linear ProPhoto* buffer
// and maps it through a print/display simulation to display sRGB. This worker
// owns that buffer (`linear_`, the preview-resolution decode), so proofing runs
// here rather than on the UI thread. See requestProof's header comment for the
// destination-key contract.

bool PreviewWorker::ensureProofProfileLoaded(const std::string& destination) {
    if (!destination.empty() && destination == proofProfileKey_)
        return proofing_.hasProofProfile();
    proofProfileKey_.clear();
    if (destination.empty())
        return false;
    // Built-in presets carry no separate .icc file in this build: ColorManager
    // serializes them in memory. `loadProofProfileFromMemory({})` clears the
    // profile, so an empty byte vector must be treated as a load failure.
    const auto loadPreset = [this](lrc::ColorManager::ExportColorProfile preset) {
        const auto bytes = lrc::ColorManager::exportProfileBytes(preset);
        return !bytes.empty() && proofing_.loadProofProfileFromMemory(bytes);
    };
    const QString key = QString::fromStdString(destination);
    bool ok = false;
    if (key.compare(QLatin1String("srgb"), Qt::CaseInsensitive) == 0) {
        ok = loadPreset(lrc::ColorManager::ExportColorProfile::SRGB);
    } else if (key.compare(QLatin1String("adobe_rgb"), Qt::CaseInsensitive) == 0) {
        ok = loadPreset(lrc::ColorManager::ExportColorProfile::AdobeRGB);
    } else if (key.compare(QLatin1String("display_p3"), Qt::CaseInsensitive) == 0) {
        ok = loadPreset(lrc::ColorManager::ExportColorProfile::DisplayP3);
    } else {
        // Everything else is a real .icc/.icm path picked in the dialog.
        ok = proofing_.loadProofProfile(toStdPath(key));
    }
    if (ok)
        proofProfileKey_ = destination;
    return ok;
}

bool PreviewWorker::applyProofState(const lrc::SoftProofParams& params,
                                    bool enabled) {
    proofParams_ = params;
    proofEnabled_ = enabled;
    if (!enabled || params.destinationProfilePath.empty())
        return false;
    // The alarm colour is engine instance state and tints both the preview
    // overlay and the exported gamut report, so it is pushed on every change
    // rather than only when a report is requested.
    proofing_.setGamutAlarmCodes(unitToU8(params.outOfGamutR),
                                 unitToU8(params.outOfGamutG),
                                 unitToU8(params.outOfGamutB));
    // A destination this build cannot resolve (the CMYK presets need an external
    // .icc) leaves proofing unusable; callers then fall back to the unproofed
    // frame and requestProof reports it.
    if (!ensureProofProfileLoaded(params.destinationProfilePath))
        return false;
    return proofArmed();
}

lrc::ImageBufferPtr PreviewWorker::proofLinear(
    const lrc::ImageBufferPtr& linearDisplay) {
    if (!proofArmed() || !linearDisplay || linearDisplay->empty())
        return nullptr;
    return proofing_.proofToDisplay(*linearDisplay, proofParams_.intent,
                                    proofParams_.showOutOfGamut);
}

uint64_t PreviewWorker::proofFrameSalt() const {
    if (!proofArmed())
        return 0;
    const auto alarm = proofing_.gamutAlarmCodes();
    uint64_t h = 0x9E3779B97F4A7C15ull;
    h ^= static_cast<uint64_t>(proofParams_.intent) * 0x100000001B3ull;
    h ^= proofParams_.showOutOfGamut ? 0xC2B2AE3D27D4EB4Full : 0ull;
    h ^= (static_cast<uint64_t>(alarm.r) << 16) |
         (static_cast<uint64_t>(alarm.g) << 8) | static_cast<uint64_t>(alarm.b);
    return h;
}

void PreviewWorker::setProofState(const lrc::SoftProofParams& params,
                                  bool enabled) {
    // Records the state only: the frames currently on screen are refreshed by the
    // requestProof() the UI issues alongside this call (preview) and by the next
    // 1:1 render (which MainWindow re-requests when the viewport shows full-res).
    applyProofState(params, enabled);
}

void PreviewWorker::requestProof(qint64 imageId, qint64 generation,
                                 lrc::AdjustmentsPtr adjustmentsPtr,
                                 const lrc::SoftProofParams& params,
                                 bool enabled) {
    if (!adjustmentsPtr)
        return;  // defensive: the UI always publishes a stack.
    const lrc::Adjustments& adjustments = *adjustmentsPtr;
    if (abort_.load())
        return;  // abort requested.
    if (imageId != loadedImageId_ || !linear_)
        return;  // only the working image can be proofed.
    latestGeneration_ = generation;
    if (!enabled) {
        // Lossless off-switch: re-emit the normal preview (and its histogram)
        // through the regular path so the unproofed frame comes back exactly.
        applyProofState(params, false);
        adjustPreview(imageId, generation, adjustmentsPtr);
        return;
    }
    if (params.destinationProfilePath.empty() ||
        !applyProofState(params, true)) {
        emit proofFailed(
            tr("无法应用软打样：目标配置文件「%1」不可用（CMYK 打样预设需要外部 ICC 文件）")
                .arg(QString::fromStdString(params.destinationProfilePath)));
        return;
    }
    // The linear display buffer is only handed back by renderPreview() on an
    // incremental-cache miss, so render it directly instead. `linear_` is
    // already the preview-resolution decode, so this stays inside the preview
    // budget; spatial kernels scale to the preview size rather than the native
    // long edge, which is irrelevant for the colourimetric proof itself.
    lrc::ImageBufferPtr linearDisplay = pipeline_.renderLinearFull(
        static_cast<uint64_t>(imageId), *linear_, adjustments);
    if (abort_.load() || !linearDisplay || linearDisplay->empty())
        return;
    lrc::ImageBufferPtr out = proofLinear(linearDisplay);
    if (!out || out->empty())
        return;
    if (abort_.load() || generation != latestGeneration_)
        return;  // superseded by a newer request.
    emit proofReady(imageId, out);
    // proofToDisplay() already returns display-ready sRGB U8, which the
    // histogram helper bins directly (no re-encoding).
    emit histogramReady(imageId,
                        lrc::Pipeline::computeHistogramFromLinear(*out));
}

void PreviewWorker::requestGamutReport(qint64 imageId,
                                       lrc::AdjustmentsPtr adjustmentsPtr,
                                       const lrc::SoftProofParams& params) {
    if (!adjustmentsPtr)
        return;  // defensive: the UI always publishes a stack.
    const lrc::Adjustments& adjustments = *adjustmentsPtr;
    if (abort_.load())
        return;
    if (imageId != loadedImageId_ || !linear_)
        return;
    if (params.destinationProfilePath.empty() ||
        !ensureProofProfileLoaded(params.destinationProfilePath)) {
        emit proofFailed(tr("无法生成色域报告：软打样目标配置文件不可用"));
        return;
    }
    // gamutMask() recognises out-of-gamut pixels by matching the *configured*
    // alarm colour, so the report must push the colour it is about to paint with
    // (the dialog can change it without arming the render paths).
    proofing_.setGamutAlarmCodes(unitToU8(params.outOfGamutR),
                                 unitToU8(params.outOfGamutG),
                                 unitToU8(params.outOfGamutB));
    lrc::ImageBufferPtr linearDisplay = pipeline_.renderLinearFull(
        static_cast<uint64_t>(imageId), *linear_, adjustments);
    if (abort_.load() || !linearDisplay || linearDisplay->empty())
        return;
    lrc::ImageBufferPtr mask =
        proofing_.gamutMask(*linearDisplay, params.intent);
    if (!mask || mask->empty()) {
        emit proofFailed(tr("无法生成色域报告：没有可用的软打样变换"));
        return;
    }
    // gamutMask() is F32 grey (1.0 = out of gamut). Paint it as a U8 RGB frame
    // using the alarm colour picked in the dialog so the PNG is directly
    // readable; MainWindow writes it losslessly.
    const int w = mask->width();
    const int h = mask->height();
    auto out = lrc::ImageBuffer::create(w, h, lrc::PixelFormat::U8);
    if (!out || out->empty())
        return;
    const float* src = reinterpret_cast<const float*>(mask->data().data());
    uint8_t* dst = out->data().data();
    const uint8_t ar = unitToU8(params.outOfGamutR);
    const uint8_t ag = unitToU8(params.outOfGamutG);
    const uint8_t ab = unitToU8(params.outOfGamutB);
    const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < pixels; ++i) {
        const bool outOfGamut = src[i * 3] > 0.5f;
        const size_t di = i * 3;
        dst[di + 0] = outOfGamut ? ar : 0;
        dst[di + 1] = outOfGamut ? ag : 0;
        dst[di + 2] = outOfGamut ? ab : 0;
    }
    emit gamutReportReady(out);
}

// --- Full-resolution 1:1 path -------------------------------------------------

void PreviewWorker::loadFullRes(qint64 imageId, const QString& filePath,
                                qint64 generation,
                                lrc::AdjustmentsPtr adjustmentsPtr) {
    if (!adjustmentsPtr)
        return;  // defensive: the UI always publishes a stack.
    const lrc::Adjustments& adjustments = *adjustmentsPtr;
    if (abort_.load())
        return;  // abort requested.
    fullResLatest_ = generation;
    // [PERF1A] worker-side entry stamp. Attribution only: the anchor verdict is
    // the end-to-end delta measured in MainWindow (t0 -> t7).
    const qint64 perfT1 = perfStampMs();
    const uint64_t adjHash = static_cast<uint64_t>(adjustments.hash());
    // Frame identity = adjustments *and* the proof state, because proofing changes
    // the pixels for an unchanged adjustment stack.
    const uint64_t frameKey = adjHash ^ proofFrameSalt();
    // Skip when the full-res frame was already emitted for this (image, state).
    if (fullResLinear_ && fullResImageId_ == imageId &&
        fullResAdjHash_ == frameKey) {
        // hit=earlyReturn renders nothing, so it is excluded from the B-1 anchor
        // verdict: a ~0 ms "second entry" is not evidence of a fast hot path.
        qCInfo(lrcPerf, "[PERF1A] hit=earlyReturn id=%lld gen=%lld",
              static_cast<long long>(imageId),
              static_cast<long long>(generation));
        return;
    }
    // Serve from the import-time full-res cache when at default settings. Those
    // blobs (and the legacy disk cache) hold unproofed pixels, so the fast path
    // is skipped while proofing is armed.
    const bool atDefaults =
        adjHash == static_cast<uint64_t>(lrc::Adjustments::defaults().hash());
    if (atDefaults && !proofArmed() &&
        !(fullResLinear_ && fullResImageId_ == imageId)) {
        // Embedded full-res JPEG stored in the catalog file; fast path, no raw
        // decode.
        if (auto blob =
                catalog_.loadPreview(imageId, lrc::PreviewKind::FullRes)) {
            auto img = lrc::ImageIO::loadImage(blob->data(), blob->size());
            if (img && !img->empty()) {
                fullResFromCache_ = true;
                fullResImageId_ = imageId;
                fullResPath_ = filePath;
                fullResCoarseHash_ = 0; // image switch invalidates coarse dedup.
                // [PERF1A] fastest hot path: catalog-embedded 1:1 preview, no
                // raw decode and no pipeline render.
                qCInfo(lrcPerf, "[PERF1A] hit=embedded id=%lld gen=%lld worker=%lldms",
                      static_cast<long long>(imageId),
                      static_cast<long long>(generation),
                      static_cast<long long>(perfStampMs() - perfT1));
                if (generation == fullResLatest_)
                    emit fullResReady(imageId, img);
                return; // embedded full-res from the catalog file (fast path).
            }
        }
        // Legacy catalogs fall back to the disk cache.
        if (auto cached = fullResCache_.findFullRes(toStdPath(filePath))) {
            auto img = lrc::ImageIO::loadImage(*cached);
            if (img && !img->empty()) {
                fullResFromCache_ = true;
                fullResImageId_ = imageId;
                fullResPath_ = filePath;
                fullResCoarseHash_ = 0; // image switch invalidates coarse dedup.
                // [PERF1A] legacy disk-cache hot path: JPEG read + decode, still
                // no raw decode.
                qCInfo(lrcPerf, "[PERF1A] hit=disk id=%lld gen=%lld worker=%lldms",
                      static_cast<long long>(imageId),
                      static_cast<long long>(generation),
                      static_cast<long long>(perfStampMs() - perfT1));
                if (generation == fullResLatest_)
                    emit fullResReady(imageId, img);
                return; // 1:1 from disk cache, no raw decode (fast).
            }
        }
    }
    // Reuse a previously rendered (image, adjHash) full-res U8, skipping the
    // decode + renderLinearFull + toDisplaySRGB pass for non-default adjustments.
    // The cache only ever holds unproofed frames, so it is bypassed while
    // proofing is armed.
    if (!atDefaults && !proofArmed()) {
        const bool haveLinear = fullResLinear_ && fullResImageId_ == imageId;
        const int nativeW = haveLinear ? fullResLinear_->width() : 0;
        const int nativeH = haveLinear ? fullResLinear_->height() : 0;
        if (auto cached = findFullResU8(imageId, adjHash, nativeW, nativeH)) {
            fullResFromCache_ = !haveLinear; // no linear; next adjust decodes lazily.
            fullResImageId_ = imageId;
            fullResPath_ = filePath;
            fullResCoarseHash_ = 0; // image/adjust change invalidates dedup.
            fullResAdjHash_ = frameKey;
            // [PERF1A] in-memory full-res U8 reuse (non-default adjustments).
            qCInfo(lrcPerf, "[PERF1A] hit=memU8 id=%lld gen=%lld worker=%lldms",
                  static_cast<long long>(imageId),
                  static_cast<long long>(generation),
                  static_cast<long long>(perfStampMs() - perfT1));
            if (generation == fullResLatest_)
                emit fullResReady(imageId, cached);
            return;
        }
    }
    if (!fullResLinear_ || fullResImageId_ != imageId) {
        if (!decodeFullRes(imageId, filePath)) {
            if (generation == fullResLatest_ && !abort_.load())
                emit loadFailed(filePath);
            return;
        }
        if (abort_.load())
            return;  // abort after the full-res decode.
    }
    // [PERF1A] cold-leg attribution: the decode is usually the dominant cost.
    qCInfo(lrcPerf, "[PERF1A] decode=done id=%lld worker=%lldms",
          static_cast<long long>(imageId),
          static_cast<long long>(perfStampMs() - perfT1));
    auto f32 = pipeline_.renderLinearFull(imageId, *fullResLinear_, adjustments);
    // [PERF1A] engine render leg (linear -> full-size), excludes display convert.
    qCInfo(lrcPerf, "[PERF1A] render=done id=%lld worker=%lldms",
          static_cast<long long>(imageId),
          static_cast<long long>(perfStampMs() - perfT1));
    if (abort_.load())
        return;  // abort during the full-res render.
    if (!f32 || f32->empty()) {
        emit loadFailed(filePath);
        return;
    }
    // Soft proofing applies to the 1:1 view as well: the full-res viewport shows
    // this frame and never the preview-sized proof frame, so without proofing
    // here the 1:1 view would silently switch to unproofed colours.
    auto u8 = proofArmed() ? proofLinear(f32) : colorManager_->toDisplaySRGB(*f32);
    if (abort_.load())
        return;  // abort during the display conversion / proof.
    if (!u8 || u8->empty()) {
        emit loadFailed(filePath);
        return;
    }
    fullResFromCache_ = false;
    if (atDefaults && !proofArmed() && generation == fullResLatest_) {
        // Store the rendered 1:1 JPEG inside the catalog file as the embedded
        // preview. Never while proofing: these blobs are the unproofed frame and
        // are reused verbatim on the next 1:1 entry.
        auto bytes = lrc::ImageIO::encodeJpeg(*u8, 92);
        if (!bytes.empty())
            catalog_.storePreview(imageId, lrc::PreviewKind::FullRes, bytes,
                                  u8->width(), u8->height());
        fullResCache_.writeFullRes(toStdPath(filePath), *u8); // legacy disk cache
    }
    if (!atDefaults && !proofArmed())
        storeFullResU8(imageId, adjHash, u8); // cache the frame.
    fullResAdjHash_ = frameKey;
    if (generation == fullResLatest_)
        emit fullResReady(imageId, u8);
    // [PERF1A] worker-side completion; the frame is queued to the UI thread here.
    qCInfo(lrcPerf, "[PERF1A] worker=done id=%lld gen=%lld worker=%lldms",
          static_cast<long long>(imageId),
          static_cast<long long>(generation),
          static_cast<long long>(perfStampMs() - perfT1));
}

void PreviewWorker::adjustFullRes(qint64 imageId, qint64 generation,
                                  lrc::AdjustmentsPtr adjustmentsPtr) {
    if (!adjustmentsPtr)
        return;  // defensive: the UI always publishes a stack.
    const lrc::Adjustments& adjustments = *adjustmentsPtr;
    if (abort_.load())
        return;  // abort requested; skip stale queued full-res re-renders.
    fullResLatest_ = generation;
    const uint64_t adjHash = static_cast<uint64_t>(adjustments.hash());
    const uint64_t frameKey = adjHash ^ proofFrameSalt();
    // Already emitted for this (adjustments, proof state).
    if (fullResAdjHash_ == frameKey)
        return;
    const bool atDefaults =
        adjHash == static_cast<uint64_t>(lrc::Adjustments::defaults().hash());
    if (!atDefaults && !proofArmed()) {
        // Cache hit: reuse the full-res U8, skipping the decode + render +
        // conversion pass. Bypassed while proofing (unproofed cache only).
        const bool haveLinear = fullResLinear_ && fullResImageId_ == imageId;
        const int nativeW = haveLinear ? fullResLinear_->width() : 0;
        const int nativeH = haveLinear ? fullResLinear_->height() : 0;
        if (auto cached = findFullResU8(imageId, adjHash, nativeW, nativeH)) {
            fullResAdjHash_ = frameKey;
            if (generation == fullResLatest_)
                emit fullResReady(imageId, cached);
            return;
        }
    }
    // A frame served from the disk cache has no linear buffer; decode it lazily
    // on the first adjustment so the 1:1 view does not freeze.
    if (!fullResLinear_ || fullResImageId_ != imageId) {
        if (!decodeFullRes(imageId, fullResPath_))
            return;
        if (abort_.load())
            return;  // abort after the lazy decode.
    }
    // Coarse-then-refine: ship a fast low-res frame ahead of the full-res render
    // so the 1:1 viewport stays responsive while the full-res pass runs.
    emitFullResCoarse(imageId, generation, adjHash, adjustments);
    if (abort_.load())
        return;  // abort during the coarse pass.
    auto f32 = pipeline_.renderLinearFull(imageId, *fullResLinear_, adjustments);
    if (abort_.load())
        return;  // abort during the full-res render.
    if (!f32 || f32->empty())
        return;
    // 1:1 is proofed here rather than from the preview-sized proof frame: the
    // full-res viewport ignores proofReady while it is showing full resolution.
    auto u8 = proofArmed() ? proofLinear(f32) : colorManager_->toDisplaySRGB(*f32);
    if (abort_.load())
        return;  // abort during the display conversion / proof.
    if (!u8 || u8->empty())
        return;
    if (!atDefaults && !proofArmed())
        storeFullResU8(imageId, adjHash, u8); // cache the frame.
    fullResAdjHash_ = frameKey;
    if (generation == fullResLatest_)
        emit fullResReady(imageId, u8);
}

void PreviewWorker::adjustFullResCoarse(qint64 imageId, qint64 generation,
                                        lrc::AdjustmentsPtr adjustmentsPtr) {
    if (!adjustmentsPtr)
        return;  // defensive: the UI always publishes a stack.
    const lrc::Adjustments& adjustments = *adjustmentsPtr;
    if (abort_.load())
        return;  // abort requested.
    fullResLatest_ = generation;
    // Lazy decode, same as adjustFullRes.
    if (!fullResLinear_ || fullResImageId_ != imageId) {
        if (!decodeFullRes(imageId, fullResPath_))
            return;
        if (abort_.load())
            return;  // abort after the lazy decode.
    }
    const uint64_t adjHash = static_cast<uint64_t>(adjustments.hash());
    if (fullResAdjHash_ == (adjHash ^ proofFrameSalt()))
        return; // full-res frame already current for this (state)
    emitFullResCoarse(imageId, generation, adjHash, adjustments);
}

void PreviewWorker::emitFullResCoarse(qint64 imageId, qint64 generation,
                                      uint64_t adjHash,
                                      const lrc::Adjustments& adjustments) {
    if (!fullResLinear_ || fullResImageId_ != imageId)
        return; // no retained full-res linear buffer for this image
    if (fullResCoarseHash_ == (adjHash ^ proofFrameSalt()))
        return; // coarse frame already emitted for this (state).
    if (abort_.load())
        return;
    auto coarse = pipeline_.render(imageId, *fullResLinear_, adjustments,
                                   lrc::app::kCoarseMaxDim);
    if (abort_.load())
        return;  // abort during the coarse render.
    if (!coarse || coarse->empty())
        return;
    // The 1:1 coarse frame is what the user sees while the full-res pass runs, so
    // it carries the proof state too (same bounded-linear trick as the preview
    // coarse path).
    if (proofArmed()) {
        if (auto lin = pipeline_.renderLinearBounded(*fullResLinear_, adjustments,
                                                    lrc::app::kCoarseMaxDim)) {
            if (auto proofed = proofLinear(lin))
                coarse = proofed;
        }
        if (abort_.load())
            return;  // abort during the proof.
    }
    fullResCoarseHash_ = adjHash ^ proofFrameSalt();
    if (generation == fullResLatest_)
        emit fullResReadyCoarse(imageId, coarse);
}

void PreviewWorker::clearFullRes() {
    fullResLinear_.reset();
    fullResImageId_ = -1;
    fullResPath_.clear();
    fullResLatest_ = -1;
    fullResAdjHash_ = 0;
    fullResCoarseHash_ = 0;
    fullResFromCache_ = false;
}

// --- Before/After + Reference view (dual render paths) -----------------------

lrc::ImageBufferPtr PreviewWorker::renderPreviewFrame(
    qint64 imageId, const lrc::ImageBuffer& linear,
    const lrc::Adjustments& adjustments, int maxDim, int fullLongEdge) {
    // Mirror the main preview's render path: use renderPreview at viewportSize_
    // when set, else a bounded render at maxDim.
    if (!viewportSize_.isEmpty()) {
        return pipeline_.renderPreview(
            linear, adjustments,
            lrc::Size{viewportSize_.width(), viewportSize_.height()},
            fullLongEdge, static_cast<uint64_t>(imageId));
    }
    return pipeline_.render(static_cast<uint64_t>(imageId), linear, adjustments,
                            maxDim);
}

void PreviewWorker::emitPreviewWithHistogram(
    qint64 imageId, qint64 generation,
    const lrc::ImageBufferPtr& linearDisplay,
    const lrc::ImageBufferPtr& rgbU8, double renderMs) {
    if (abort_.load() || generation != latestGeneration_)
        return;  // stale request superseded by a newer one.
    emit previewReady(imageId, rgbU8);
    // Perf diagnostics: reported after the frame is on its way to the UI, so the
    // sample includes the signal emission cost of the refined frame only.
    if (renderMs >= 0.0)
        emit previewTimingReady(imageId, renderMs);
    // P3-1: derive the histogram from the adjusted linear F32 display buffer
    // (full traversal, no sampling). When the incremental cache served the
    // unchanged preview, renderPreview leaves linearDisplay null -- fall back
    // to the U8 frame, which is also traversed in full. Both signals are
    // emitted in this order so the UI lands on the linear-light histogram.
    const lrc::ImageBufferPtr& src =
        (linearDisplay && !linearDisplay->empty()) ? linearDisplay : rgbU8;
    if (src)
        emit histogramReady(
            imageId, lrc::Pipeline::computeHistogramFromLinear(*src));
}

QString PreviewWorker::pathForImage(qint64 imageId) const {
    // CatalogDatabase has no by-id path lookup, so scan listImages() which
    // carries every record's file path. Reference/unloaded images are one-off
    // requests, so O(n) is acceptable.
    const auto images = catalog_.listImages();
    for (const auto& rec : images) {
        if (rec.id == imageId)
            return QString::fromStdString(rec.filePath);
    }
    return QString();
}

void PreviewWorker::requestBeforeAfter(qint64 imageId, qint64 generation,
                                       lrc::AdjustmentsPtr afterAdjPtr,
                                       lrc::AdjustmentsPtr beforeAdjPtr,
                                       int maxDim, bool halfSize) {
    if (!afterAdjPtr || !beforeAdjPtr)
        return;  // defensive: the UI always publishes both stacks.
    const lrc::Adjustments& afterAdj = *afterAdjPtr;
    const lrc::Adjustments& beforeAdj = *beforeAdjPtr;
    if (abort_.load())
        return;  // abort requested.
    // Joins the main generation guard; a newer request supersedes this one.
    latestGeneration_ = generation;
    // Prefer the loaded preview-res buffer. Otherwise decode a local preview-res
    // copy; never write back to linear_, so the main preview state is left
    // intact.
    lrc::ImageBufferPtr src = linear_;
    int srcLongEdge = fullLongEdge_;
    if (imageId != loadedImageId_ || !src) {
        const QString path = pathForImage(imageId);
        if (path.isEmpty())
            return;  // image not in the catalog.
        lrc::DecodeParams params;
        params.halfSize = halfSize;
        params.maxDim = maxDim > 0 ? maxDim : lrc::app::kPreviewMaxDim;
        auto decoded = decoder_.decode(toStdPath(path), params);
        if (!decoded || abort_.load())
            return;  // decode failed / aborted.
        src = decoded->linear;
        srcLongEdge = std::max(decoded->meta.nativeWidth,
                               decoded->meta.nativeHeight);
    }
    if (!src || src->empty())
        return;
    // Render the After state with the current adjustment stack.
    auto after = renderPreviewFrame(imageId, *src, afterAdj, maxDim,
                                    srcLongEdge);
    if (abort_.load() || !after || after->empty())
        return;  // After render failed / aborted.
    // Render the Before state with the zero-adjustment default stack. Reuse the
    // single After render when the stacks are identical.
    lrc::ImageBufferPtr before = after;
    if (beforeAdj != afterAdj) {
        before = renderPreviewFrame(imageId, *src, beforeAdj, maxDim,
                                    srcLongEdge);
        if (abort_.load() || !before || before->empty())
            return;  // Before render failed / aborted.
    }
    if (generation != latestGeneration_)
        return;  // stale: superseded.
    emit beforeAfterReady(imageId, toQImage(before), toQImage(after));
}

void PreviewWorker::requestReference(qint64 refImageId, qint64 generation,
                                     lrc::AdjustmentsPtr refAdjPtr,
                                     int maxDim) {
    if (!refAdjPtr)
        return;  // defensive: the UI always publishes a stack.
    const lrc::Adjustments& refAdj = *refAdjPtr;
    if (abort_.load())
        return;  // abort requested.
    // The reference render uses its own generation guard and never touches the
    // working image state, so the two paths cannot overwrite each other.
    refLatestGeneration_ = generation;
    const QString path = pathForImage(refImageId);
    if (path.isEmpty()) {
        if (generation == refLatestGeneration_)
            emit referenceReady(refImageId, QImage());  // absent from catalog.
        return;
    }
    lrc::ImageBufferPtr refLinear;
    int refFullLongEdge = 0;  // standard-preview v1 lacks native size -> 0 = source-relative.
    // Prefer the import-time standard preview (2048px linear ProPhoto F32),
    // skipping the raw decode. v2 blobs carry the native dimensions so the
    // reference render's kernel scaling is correct too; v1 blobs leave
    // refFullLongEdge at 0 (source-relative).
    if (catalog_.hasStandardPreview(refImageId)) {
        if (auto sp = catalog_.getStandardPreviewWithMeta(refImageId)) {
            if (sp->buf && !sp->buf->empty()) {
                refLinear = sp->buf;
                if (sp->nativeWidth > 0 && sp->nativeHeight > 0)
                    refFullLongEdge = std::max(sp->nativeWidth,
                                               sp->nativeHeight);
            }
        }
    }
    if (!refLinear) {
        lrc::DecodeParams params;
        params.halfSize = true;
        params.maxDim = maxDim > 0 ? maxDim : lrc::app::kPreviewMaxDim;
        auto decoded = decoder_.decode(toStdPath(path), params);
        if (!decoded) {
            if (generation == refLatestGeneration_)
                emit referenceReady(refImageId, QImage());  // decode failed.
            return;
        }
        if (abort_.load())
            return;  // abort after the decode.
        refLinear = decoded->linear;
        refFullLongEdge = std::max(decoded->meta.nativeWidth,
                                   decoded->meta.nativeHeight);
    }
    if (!refLinear || refLinear->empty()) {
        if (generation == refLatestGeneration_)
            emit referenceReady(refImageId, QImage());
        return;
    }
    auto out = renderPreviewFrame(refImageId, *refLinear, refAdj, maxDim,
                                  refFullLongEdge);
    if (abort_.load())
        return;  // abort during the reference render.
    if (!out || out->empty()) {
        if (generation == refLatestGeneration_)
            emit referenceReady(refImageId, QImage());  // render failed.
        return;
    }
    if (generation == refLatestGeneration_)
        emit referenceReady(refImageId, toQImage(out));
}

// --- Full-res display U8 LRU cache helpers ------------------------------------

lrc::ImageBufferPtr PreviewWorker::findFullResU8(qint64 imageId,
                                                 uint64_t adjHash, int w,
                                                 int h) {
    for (auto it = fullResU8Cache_.begin(); it != fullResU8Cache_.end();) {
        if (it->imageId != imageId || it->adjHash != adjHash) {
            ++it;
            continue;
        }
        if (!it->u8 || it->u8->empty()) {
            it = fullResU8Cache_.erase(it); // drop empty entries.
            continue;
        }
        // When the reference native size is known, a mismatch invalidates the
        // entry (stale size).
        if ((w > 0 && it->width != w) || (h > 0 && it->height != h)) {
            it = fullResU8Cache_.erase(it); // stale size.
            continue;
        }
        lrc::ImageBufferPtr result = it->u8; // hold the hit buffer.
        FullResU8Entry hit = std::move(*it);  // promote to most-recently-used LRU.
        fullResU8Cache_.erase(it);
        fullResU8Cache_.insert(fullResU8Cache_.begin(), std::move(hit));
        return result;
    }
    return nullptr;
}

void PreviewWorker::storeFullResU8(qint64 imageId, uint64_t adjHash,
                                   const lrc::ImageBufferPtr& u8) {
    if (!u8 || u8->empty())
        return;
    if (adjHash ==
        static_cast<uint64_t>(lrc::Adjustments::defaults().hash()))
        return; // defaults are never cached.
    // Drop a same-key stale entry first to avoid duplicates.
    fullResU8Cache_.erase(
        std::remove_if(fullResU8Cache_.begin(), fullResU8Cache_.end(),
                       [imageId, adjHash](const FullResU8Entry& e) {
                           return e.imageId == imageId && e.adjHash == adjHash;
                       }),
        fullResU8Cache_.end());
    // Evict the least-recently-used entry when at capacity.
    if (static_cast<int>(fullResU8Cache_.size()) >= kFullResU8CacheSize)
        fullResU8Cache_.pop_back();
    FullResU8Entry entry;
    entry.imageId = imageId;
    entry.adjHash = adjHash;
    entry.width = u8->width();
    entry.height = u8->height();
    entry.u8 = u8;
    fullResU8Cache_.insert(fullResU8Cache_.begin(), std::move(entry));
}

bool PreviewWorker::decodeFullRes(qint64 imageId, const QString& filePath) {
    lrc::DecodeParams params; // full resolution: halfSize=false, maxDim=0
    auto decoded = decoder_.decode(toStdPath(filePath), params);
    if (!decoded) {
        // Release the stale full-res buffer.
        fullResLinear_.reset();
        fullResImageId_ = -1;
        fullResPath_.clear();
        fullResAdjHash_ = 0;
        fullResCoarseHash_ = 0;
        fullResFromCache_ = false;
        return false;
    }
    fullResLinear_ = decoded->linear;
    fullResImageId_ = imageId;
    fullResPath_ = filePath;
    fullLongEdge_ = std::max(decoded->meta.nativeWidth,
                             decoded->meta.nativeHeight);
    fullResCoarseHash_ = 0; // new image invalidates coarse dedup.
    fullResFromCache_ = false; // linear buffer available now.
    return true;
}

void PreviewWorker::exportImage(qint64 imageId, const QString& filePath,
                                lrc::AdjustmentsPtr adjustmentsPtr,
                                const QString& outPath, int format,
                                int quality, int maxDim, int icc,
                                const ExportWatermark& watermark) {
    if (!adjustmentsPtr)
        return;  // defensive: the UI always publishes a stack.
    const lrc::Adjustments& adjustments = *adjustmentsPtr;
    // Route the single-shot export through the unified ExportEngine::encodeAndWrite
    // entry point so the size preset (maxDim) from ExportDialog is applied in
    // linear F32 space before the format-specific encode. The dispatch is the
    // same as the legacy switch but collapses five branches into one call.
    // maxDim=0 keeps native resolution (a no-op resize). `icc` / `watermark`
    // carry the remaining export options.
    if (abort_.load())
        return;  // abort requested.
    lrc::DecodeParams params; // full resolution
    auto decoded = decoder_.decode(toStdPath(filePath), params,
                                   dcp_ ? &*dcp_ : nullptr);
    if (!decoded) {
        emit exportFinished(false, tr("无法解码原始文件: %1").arg(filePath));
        return;
    }
    lrc::ExportEngine::ExportSpec spec;
    switch (static_cast<ExportFormat>(format)) {
    case ExportFormat::Jpeg:
        spec.format = lrc::ExportEngine::ExportSpec::Format::Jpeg; break;
    case ExportFormat::Png:
        spec.format = lrc::ExportEngine::ExportSpec::Format::Png; break;
    case ExportFormat::Tiff8:
        spec.format = lrc::ExportEngine::ExportSpec::Format::Tiff8; break;
    case ExportFormat::Tiff16:
        spec.format = lrc::ExportEngine::ExportSpec::Format::Tiff16; break;
    case ExportFormat::Dng:
        spec.format = lrc::ExportEngine::ExportSpec::Format::Dng; break;
    }
    spec.quality = quality;
    spec.maxDim = maxDim;
    applyExportOptions(spec, icc, watermark);

    const bool ok = exportEngine_->encodeAndWrite(
        toStdPath(outPath), static_cast<uint64_t>(imageId),
        *decoded->linear, adjustments, spec);
    emit exportFinished(ok, ok ? tr("导出成功") : tr("导出失败: %1")
                                            .arg(QString::fromStdString(
                                                exportEngine_->lastError())));
}

void PreviewWorker::batchExport(const QList<ExportItem>& items) {
    // Serial decode + encode + write per item. The export bottleneck is the
    // encode pass, so a serial loop keeps memory bounded (one linear buffer at
    // a time) while the per-strip work still saturates the pool. Aborting
    // mid-batch breaks the loop; the counts then reflect only the items actually
    // attempted. lastError carries the most recent failure (empty when every
    // item succeeded).
    int success = 0;
    int fail = 0;
    std::string lastError;
    const int total = items.size();

    for (int i = 0; i < total; ++i) {
        if (abort_.load())
            break;  // abort mid-batch.

        const ExportItem& item = items[i];

        // A null snapshot means the producer failed to freeze the stack. Fail
        // this item instead of rendering with defaults (which would silently
        // export the wrong pixels); the remaining items still run.
        if (!item.adjustments) {
            lastError = "missing adjustments: " + item.filePath.toStdString();
            ++fail;
            emit batchExportProgress(i + 1, total);
            continue;
        }

        // Map the UI-side ExportFormat onto ExportEngine's ExportSpec. Same
        // ordering as exportImage's switch, but routed through encodeAndWrite
        // so maxDim (if set) is applied uniformly before the format dispatch.
        lrc::ExportEngine::ExportSpec spec;
        switch (item.format) {
        case ExportFormat::Jpeg:
            spec.format = lrc::ExportEngine::ExportSpec::Format::Jpeg; break;
        case ExportFormat::Png:
            spec.format = lrc::ExportEngine::ExportSpec::Format::Png; break;
        case ExportFormat::Tiff8:
            spec.format = lrc::ExportEngine::ExportSpec::Format::Tiff8; break;
        case ExportFormat::Tiff16:
            spec.format = lrc::ExportEngine::ExportSpec::Format::Tiff16; break;
        case ExportFormat::Dng:
            spec.format = lrc::ExportEngine::ExportSpec::Format::Dng; break;
        }
        spec.quality = item.quality;
        spec.maxDim = item.maxDim;
        applyExportOptions(spec, item.icc, item.watermark);

        // Full-resolution decode (DecodeParams{} == halfSize=false, maxDim=0, i.e.
        // native resolution; the export path must never use the cached
        // preview-resolution buffer). A decode failure counts as a per-item
        // failure rather than aborting the whole batch -- the remaining items
        // may still be perfectly exportable.
        lrc::DecodeParams params;  // full resolution.
        auto decoded = decoder_.decode(toStdPath(item.filePath), params);
        bool ok = false;
        if (decoded && decoded->linear && !decoded->linear->empty()) {
            ok = exportEngine_->encodeAndWrite(
                toStdPath(item.outPath),
                static_cast<uint64_t>(item.imageId),
                *decoded->linear, *item.adjustments, spec);
            if (!ok)
                lastError = exportEngine_->lastError();
        } else {
            lastError = "decode failed: " + item.filePath.toStdString();
        }

        if (ok)
            ++success;
        else
            ++fail;

        emit batchExportProgress(i + 1, total);
    }

    emit batchExportFinished(success, fail,
                             QString::fromStdString(lastError));
}

void PreviewWorker::computeAutoAdjust(qint64 imageId,
                                      const QString& filePath) {
    if (abort_.load())
        return;  // abort requested.
    lrc::ImageBufferPtr linear = linear_;
    if (!linear || loadedImageId_ != imageId) {
        lrc::DecodeParams params;
        params.halfSize = true;
        params.maxDim = lrc::app::kPreviewMaxDim;
        auto decoded = decoder_.decode(toStdPath(filePath), params);
        if (!decoded) {
            emit loadFailed(filePath);
            return;
        }
        linear = decoded->linear;
    }
    // B-1 auto-develop: exposure/WB come from autoAdjust; contrast (not part of
    // autoAdjust) is taken from autoDevelop. The crop suggestion has no
    // Adjustments field today, so it is intentionally not written back here --
    // instead it is surfaced as a viewport guidance box via autoCropSuggestionReady.
    const auto dev = lrc::Pipeline::autoDevelop(*linear);
    lrc::Adjustments adj = lrc::Pipeline::autoAdjust(*linear);
    adj.contrast = dev.contrast;
    emit autoAdjustReady(adj);
    emit autoCropSuggestionReady(dev.hasCrop, dev.crop.left, dev.crop.top,
                                 dev.crop.right, dev.crop.bottom);
}

void PreviewWorker::renderVisualization(qint64 imageId, qint64 generation,
                                        int mode,
                                        lrc::AdjustmentsPtr adjustmentsPtr) {
    if (!adjustmentsPtr)
        return;  // defensive: the UI always publishes a stack.
    const lrc::Adjustments& adjustments = *adjustmentsPtr;
    if (abort_.load())
        return;  // abort requested.
    if (imageId != loadedImageId_ || !linear_)
        return;
    latestGeneration_ = generation;
    const auto vizMode = static_cast<lrc::Pipeline::VisualizeMode>(mode);
    auto overlay = pipeline_.visualizeOverlay(*linear_, adjustments, vizMode, 0);
    if (generation == latestGeneration_ && overlay && !overlay->empty())
        emit visualizationReady(imageId, generation, overlay);
}

void PreviewWorker::mergeHdr(const QStringList& paths, bool deghost,
                             const QString& outPath) {
    if (abort_.load())
        return;  // abort requested.
    lrc::DecodeParams params; // full resolution
    std::vector<lrc::ImageBufferPtr> frames;
    frames.reserve(static_cast<size_t>(paths.size()));
    for (int i = 0; i < paths.size(); ++i) {
        emit hdrProgress(i, paths.size());
        auto decoded = decoder_.decode(toStdPath(paths[i]), params);
        if (!decoded) {
            emit hdrFinished(false,
                             tr("无法解码包围照片: %1").arg(paths[i]), QString());
            return;
        }
        frames.push_back(decoded->linear);
    }
    emit hdrProgress(paths.size(), paths.size());

    const auto result = lrc::HdrMerge::merge(frames, {}, deghost);
    if (!result.ok()) {
        emit hdrFinished(
            false, tr("HDR 合并失败: %1")
                       .arg(QString::fromStdString(result.error)),
            QString());
        return;
    }
    const bool ok = exportEngine_->exportDng(
        toStdPath(outPath), 0, *result.linear, lrc::Adjustments::defaults());
    emit hdrFinished(
        ok,
        ok ? tr("HDR 合并完成，DNG 已写入")
           : tr("写入 DNG 失败: %1")
                 .arg(QString::fromStdString(exportEngine_->lastError())),
        ok ? outPath : QString());
}

void PreviewWorker::convertToDng(qint64 /*imageId*/, const QString& filePath,
                                 const QString& outPath) {
    if (abort_.load())
        return;  // abort requested.
    std::string error;
    const bool ok =
        lrc::DngConverter::convert(toStdPath(filePath), toStdPath(outPath), &error);
    emit convertFinished(
        ok, ok ? tr("已转换为 DNG: %1").arg(outPath)
               : tr("转换失败: %1").arg(QString::fromStdString(error)),
        outPath);
}
