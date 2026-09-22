#include "ui/PreviewWorker.h"

#include <algorithm>
#include <filesystem>

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
                              const lrc::Adjustments& adjustments) {
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
                                  const lrc::Adjustments& adjustments) {
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
        emitPreviewWithHistogram(imageId, generation, linearDisplay, out);
    } else {
        out = pipeline_.render(imageId, *linear_, adjustments, 0);
        if (abort_.load())
            return;  // abort during render.
        emitPreviewWithHistogram(imageId, generation, nullptr, out);
    }
}

void PreviewWorker::adjustPreviewCoarse(qint64 imageId, qint64 generation,
                                        const lrc::Adjustments& adjustments) {
    if (abort_.load())
        return;  // abort requested; skip stale queued re-renders.
    if (imageId != loadedImageId_ || !linear_)
        return;
    latestGeneration_ = generation;
    auto out = pipeline_.render(imageId, *linear_, adjustments,
                                lrc::app::kCoarseMaxDim);
    if (abort_.load())
        return;  // abort during render.
    if (generation == latestGeneration_)
        emit previewReadyCoarse(imageId, out);
}

// --- Full-resolution 1:1 path -------------------------------------------------

void PreviewWorker::loadFullRes(qint64 imageId, const QString& filePath,
                                qint64 generation,
                                const lrc::Adjustments& adjustments) {
    if (abort_.load())
        return;  // abort requested.
    fullResLatest_ = generation;
    const uint64_t adjHash = static_cast<uint64_t>(adjustments.hash());
    // Skip when the full-res frame was already emitted for this (image, adjHash).
    if (fullResLinear_ && fullResImageId_ == imageId &&
        fullResAdjHash_ == adjHash) {
        return;
    }
    // Serve from the import-time full-res cache when at default settings.
    const bool atDefaults =
        adjHash == static_cast<uint64_t>(lrc::Adjustments::defaults().hash());
    if (atDefaults && !(fullResLinear_ && fullResImageId_ == imageId)) {
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
                if (generation == fullResLatest_)
                    emit fullResReady(imageId, img);
                return; // 1:1 from disk cache, no raw decode (fast).
            }
        }
    }
    // Reuse a previously rendered (image, adjHash) full-res U8, skipping the
    // decode + renderLinearFull + toDisplaySRGB pass for non-default adjustments.
    if (!atDefaults) {
        const bool haveLinear = fullResLinear_ && fullResImageId_ == imageId;
        const int nativeW = haveLinear ? fullResLinear_->width() : 0;
        const int nativeH = haveLinear ? fullResLinear_->height() : 0;
        if (auto cached = findFullResU8(imageId, adjHash, nativeW, nativeH)) {
            fullResFromCache_ = !haveLinear; // no linear; next adjust decodes lazily.
            fullResImageId_ = imageId;
            fullResPath_ = filePath;
            fullResCoarseHash_ = 0; // image/adjust change invalidates dedup.
            fullResAdjHash_ = adjHash;
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
    auto f32 = pipeline_.renderLinearFull(imageId, *fullResLinear_, adjustments);
    if (abort_.load())
        return;  // abort during the full-res render.
    if (!f32 || f32->empty()) {
        emit loadFailed(filePath);
        return;
    }
    auto u8 = colorManager_->toDisplaySRGB(*f32);
    if (abort_.load())
        return;  // abort during the sRGB conversion.
    fullResFromCache_ = false;
    if (atDefaults && generation == fullResLatest_) {
        // Store the rendered 1:1 JPEG inside the catalog file as the embedded
        // preview.
        auto bytes = lrc::ImageIO::encodeJpeg(*u8, 92);
        if (!bytes.empty())
            catalog_.storePreview(imageId, lrc::PreviewKind::FullRes, bytes,
                                  u8->width(), u8->height());
        fullResCache_.writeFullRes(toStdPath(filePath), *u8); // legacy disk cache
    }
    if (!atDefaults)
        storeFullResU8(imageId, adjHash, u8); // cache the frame.
    fullResAdjHash_ = adjHash;
    if (generation == fullResLatest_)
        emit fullResReady(imageId, u8);
}

void PreviewWorker::adjustFullRes(qint64 imageId, qint64 generation,
                                  const lrc::Adjustments& adjustments) {
    if (abort_.load())
        return;  // abort requested; skip stale queued full-res re-renders.
    fullResLatest_ = generation;
    const uint64_t adjHash = static_cast<uint64_t>(adjustments.hash());
    // Already emitted for this adjustment state.
    if (fullResAdjHash_ == adjHash)
        return;
    const bool atDefaults =
        adjHash == static_cast<uint64_t>(lrc::Adjustments::defaults().hash());
    if (!atDefaults) {
        // Cache hit: reuse the full-res U8, skipping the decode + render +
        // conversion pass.
        const bool haveLinear = fullResLinear_ && fullResImageId_ == imageId;
        const int nativeW = haveLinear ? fullResLinear_->width() : 0;
        const int nativeH = haveLinear ? fullResLinear_->height() : 0;
        if (auto cached = findFullResU8(imageId, adjHash, nativeW, nativeH)) {
            fullResAdjHash_ = adjHash;
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
    auto u8 = colorManager_->toDisplaySRGB(*f32);
    if (abort_.load())
        return;  // abort during the sRGB conversion.
    if (!atDefaults)
        storeFullResU8(imageId, adjHash, u8); // cache the frame.
    fullResAdjHash_ = adjHash;
    if (generation == fullResLatest_)
        emit fullResReady(imageId, u8);
}

void PreviewWorker::adjustFullResCoarse(qint64 imageId, qint64 generation,
                                        const lrc::Adjustments& adjustments) {
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
    if (fullResAdjHash_ == adjHash)
        return; // full-res frame already current for this (image, adjHash)
    emitFullResCoarse(imageId, generation, adjHash, adjustments);
}

void PreviewWorker::emitFullResCoarse(qint64 imageId, qint64 generation,
                                      uint64_t adjHash,
                                      const lrc::Adjustments& adjustments) {
    if (!fullResLinear_ || fullResImageId_ != imageId)
        return; // no retained full-res linear buffer for this image
    if (fullResCoarseHash_ == adjHash)
        return; // coarse frame already emitted for this (image, adjHash).
    if (abort_.load())
        return;
    auto coarse = pipeline_.render(imageId, *fullResLinear_, adjustments,
                                   lrc::app::kCoarseMaxDim);
    if (abort_.load())
        return;  // abort during the coarse render.
    if (!coarse || coarse->empty())
        return;
    fullResCoarseHash_ = adjHash;
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
    const lrc::ImageBufferPtr& rgbU8) {
    if (abort_.load() || generation != latestGeneration_)
        return;  // stale request superseded by a newer one.
    emit previewReady(imageId, rgbU8);
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
                                       const lrc::Adjustments& afterAdj,
                                       const lrc::Adjustments& beforeAdj,
                                       int maxDim, bool halfSize) {
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
                                     const lrc::Adjustments& refAdj,
                                     int maxDim) {
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
                                const lrc::Adjustments& adjustments,
                                const QString& outPath, int format,
                                int quality, int maxDim, int icc,
                                const ExportWatermark& watermark) {
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
                *decoded->linear, item.adjustments, spec);
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
    emit autoAdjustReady(lrc::Pipeline::autoAdjust(*linear));
}

void PreviewWorker::renderVisualization(qint64 imageId, qint64 generation,
                                        int mode,
                                        const lrc::Adjustments& adjustments) {
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
