#pragma once

// Background preview + export worker. Owns the current decoded linear image
// so repeated slider changes re-render without touching the raw file again.
// Runs in its own QThread; results cross back to the UI via queued signals.

#include <atomic>
#include <vector>

#include <QImage>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>

#include "app/Constants.h"
#include "engine/Adjustments.h"
#include "engine/ImageBuffer.h"

// Export formats understood by PreviewWorker::exportImage. Index order must
// match the ExportDialog combo.
enum class ExportFormat : int {
    Jpeg = 0,
    Png,
    Tiff8,
    Tiff16,
    Dng,
};

// S17-18: one entry in a batch export. `maxDim` is the longest-edge cap in
// pixels (0 = original / native resolution). `quality` is the JPEG quality
// (0..100, ignored for lossless formats). Crosses the queued connection from
// MainWindow to PreviewWorker::batchExport; registered in main().
struct ExportItem {
    qint64 imageId = 0;
    QString filePath;
    lrc::Adjustments adjustments;
    QString outPath;
    ExportFormat format = ExportFormat::Jpeg;
    int quality = 92;
    int maxDim = 0;
};
Q_DECLARE_METATYPE(ExportItem)
Q_DECLARE_METATYPE(QList<ExportItem>)

namespace lrc {
class CatalogDatabase;
class ColorManager;
class ExportEngine;
class Pipeline;
class RawDecoder;
class ThumbnailCache;
} // namespace lrc

class PreviewWorker : public QObject {
    Q_OBJECT
public:
    PreviewWorker(lrc::RawDecoder& decoder, lrc::Pipeline& pipeline,
                  lrc::ThumbnailCache& fullResCache,
                  lrc::CatalogDatabase& catalog,
                  std::shared_ptr<lrc::ColorManager> colorManager,
                  QObject* parent = nullptr);
    ~PreviewWorker() override;

    // 请求取消:耗时槽入口检查后立即返回,不再处理排队任务 (request shutdown;
    // long-running slots bail at entry). Thread-safe.
    void requestAbort() { abort_.store(true); }

public slots:
    // PHASE_2: the viewport's logical render size (widget size x devicePixelRatio)
    // for Fit/<=1:1 views. Stored and applied by the next loadImage/adjustPreview;
    // a queued connection delivers it on the worker thread so no lock is needed.
    void setPreviewTargetSize(const QSize& size);
    // Decodes a raw at preview resolution, keeps it as the working image and
    // renders with the given adjustments.
    void loadImage(qint64 imageId, const QString& filePath,
                   qint64 generation, const lrc::Adjustments& adjustments);
    // Re-renders the working image with new adjustments (no raw decode).
    void adjustPreview(qint64 imageId, qint64 generation,
                       const lrc::Adjustments& adjustments);
    // Coarse preview re-render: same semantics as adjustPreview but bounded to
    // kCoarseMaxDim, giving the UI a fast low-res frame before the full 1920
    // preview lands (coarse-then-refine). Shares the same latestGeneration_
    // guard, so stale coarse/refine renders are dropped together.
    void adjustPreviewCoarse(qint64 imageId, qint64 generation,
                             const lrc::Adjustments& adjustments);
    // Full-resolution decode + render + write. `format` is an ExportFormat
    // value, `quality` the JPEG quality (0..100, ignored for lossless
    // formats). `maxDim` (S17-18) is the longest-edge cap; 0 keeps native
    // resolution. Emits exportFinished.
    void exportImage(qint64 imageId, const QString& filePath,
                     const lrc::Adjustments& adjustments,
                     const QString& outPath, int format, int quality,
                     int maxDim = 0);
    // S17-18: serial batch export. Each item is decoded + rendered + written
    // in turn (the export bottleneck is encode, not decode; TIFF16 is already
    // strip-parallel from S16). Emits batchExportProgress per item and a
    // single batchExportFinished at the end with aggregate success/fail
    // counts. Aborts mid-batch when requestAbort() was called; the counts
    // then reflect only the items actually attempted.
    void batchExport(const QList<ExportItem>& items);
    // Decodes (or reuses the cached linear image) and computes the auto-adjust
    // suggestion for the current photo.
    void computeAutoAdjust(qint64 imageId, const QString& filePath);
    // Renders an LR-style Alt-key diagnostic overlay (clipping / detail mask /
    // edge mask) from the working linear image. mode is
    // Pipeline::VisualizeMode as an int.
    void renderVisualization(qint64 imageId, qint64 generation, int mode,
                             const lrc::Adjustments& adjustments);
    // Decodes every bracketed frame at full resolution, merges them into a
    // linear HDR buffer (HdrMerge) and writes the result as a linear DNG.
    // On success hdrFinished carries the output path so MainWindow can import
    // the merged file back into the catalog.
    void mergeHdr(const QStringList& paths, bool deghost,
                  const QString& outPath);
    // Converts a raw to a raw (mosaic-preserving) DNG via the engine's
    // DngConverter. On success convertFinished carries the output path so
    // MainWindow can import the DNG back into the catalog.
    void convertToDng(qint64 imageId, const QString& filePath,
                      const QString& outPath);
    // --- Full-resolution 1:1 path (added to PreviewWorker) --------------------
    // Decodes at full resolution (DecodeParams{}: halfSize=false, maxDim=0),
    // renders to display sRGB and retains the full-res linear buffer as the
    // working image. Emits fullResReady when this generation is still current.
    void loadFullRes(qint64 imageId, const QString& filePath,
                     qint64 generation, const lrc::Adjustments& adjustments);
    // Re-renders the retained full-res linear buffer with new adjustments
    // (no decode). No-op unless a full-res image is loaded.
    void adjustFullRes(qint64 imageId, qint64 generation,
                       const lrc::Adjustments& adjustments);
    // 1:1 coarse intermediate: re-renders the retained full-res linear buffer
    // bounded to kCoarseMaxDim (fast low-res frame) without touching the raw
    // file again. Emits fullResReadyCoarse when this generation is current.
    // Mirrors adjustPreviewCoarse; MainWindow throttles this during 1:1 drags
    // (Phase 2 wiring). adjustFullRes ships its own coarse frame first, so
    // leaving this slot unwired degrades safely to coarse-then-refine on the
    // refine path.
    void adjustFullResCoarse(qint64 imageId, qint64 generation,
                             const lrc::Adjustments& adjustments);
    // Releases the retained full-res buffer(s). Called on exit 1:1 / image switch.
    void clearFullRes();

    // --- M7: Before/After + Reference view -----------------------------------
    // Renders BOTH the After (current adjustments) and Before (usually the
    // zero-adjustment / default snapshot) states of the same working image, then
    // emits beforeAfterReady when both complete. The two passes reuse the loaded
    // preview-resolution linear buffer and the pipeline caches (the Before pass
    // with defaults typically hits the default-adjustment cache). This request
    // joins the same generation guard as loadImage/adjustPreview: a newer
    // request supersedes it, while the internal After/Before pair share one
    // generation and never invalidate each other. When After and Before are the
    // same adjustment stack, the single render is reused for both.
    void requestBeforeAfter(qint64 imageId, qint64 generation,
                            const lrc::Adjustments& afterAdj,
                            const lrc::Adjustments& beforeAdj,
                            int maxDim = lrc::app::kPreviewMaxDim,
                            bool halfSize = true);
    // Renders an arbitrary library photo (by catalog id) at preview resolution
    // with its own adjustment stack and emits referenceReady. Prefers the
    // import-time standard preview when available (no raw decode). Runs on an
    // independent generation guard (refLatestGeneration_) and never touches the
    // working image's linear_/latestGeneration_, so a reference render cannot
    // collide with -- or be clobbered by -- the main preview path.
    void requestReference(qint64 refImageId, qint64 generation,
                          const lrc::Adjustments& refAdj,
                          int maxDim = lrc::app::kPreviewMaxDim);

signals:
    void previewReady(qint64 imageId, const lrc::ImageBufferPtr& rgbU8);
    // Low-res (kCoarseMaxDim) preview frame, delivered ahead of previewReady.
    void previewReadyCoarse(qint64 imageId, const lrc::ImageBufferPtr& rgbU8);
    // Native pixel dimensions of the decoded image (before any preview
    // downscale). Emitted once per successful decode; lets the 1:1 viewport
    // map the downscaled preview at true full size while full-res loads.
    void imageMeta(qint64 imageId, int nativeWidth, int nativeHeight);
    void loadFailed(const QString& filePath);
    void exportFinished(bool ok, const QString& message);
    // S17-18: batch export progress (1-based done count, total == items.size()).
    void batchExportProgress(int done, int total);
    // S17-18: aggregate result of a batch export. `lastError` carries the
    // engine's last error message when any item failed (empty on full success).
    void batchExportFinished(int successCount, int failCount,
                             const QString& lastError);
    void autoAdjustReady(const lrc::Adjustments& adjustments);
    void visualizationReady(qint64 imageId, qint64 generation,
                            const lrc::ImageBufferPtr& overlay);
    void hdrProgress(int done, int total);
    void hdrFinished(bool ok, const QString& message, const QString& outPath);
    void convertFinished(bool ok, const QString& message, const QString& outPath);
    // Full-resolution display-ready sRGB buffer (RGB U8). Same shape as
    // previewReady — ImageBufferPtr is already a registered metatype.
    void fullResReady(qint64 imageId, const lrc::ImageBufferPtr& rgbU8);
    // Low-res (kCoarseMaxDim) 1:1 frame delivered ahead of fullResReady so the
    // full-res viewport gets immediate drag feedback while the full-res render
    // runs (coarse-then-refine, mirror of previewReadyCoarse).
    void fullResReadyCoarse(qint64 imageId, const lrc::ImageBufferPtr& rgbU8);
    // M7: both Before and After previews for one Before/After view are ready.
    // Both are display-ready sRGB QImages at the current preview resolution.
    // When the Before and After adjustment stacks are identical, `before` is
    // the same buffer as `after`.
    void beforeAfterReady(qint64 imageId, const QImage& before,
                          const QImage& after);
    // M7: the reference photo's preview is ready. A null QImage signals that the
    // reference could not be produced (image not in the catalog, or a decode /
    // render failure).
    void referenceReady(qint64 refImageId, const QImage& image);

private:
    // Decodes the raw at full resolution into fullResLinear_ (shared by
    // loadFullRes and adjustFullRes' lazy decode). False on decode failure.
    bool decodeFullRes(qint64 imageId, const QString& filePath);
    // Renders the 1:1 coarse intermediate (kCoarseMaxDim) from fullResLinear_
    // and emits fullResReadyCoarse. Dedups on fullResCoarseHash_ and never
    // touches fullResAdjHash_ (the full-res frame state), so the coarse frame
    // cannot suppress the subsequent full render.
    void emitFullResCoarse(qint64 imageId, qint64 generation, uint64_t adjHash,
                           const lrc::Adjustments& adjustments);

    lrc::RawDecoder& decoder_;
    lrc::Pipeline& pipeline_;
    lrc::ThumbnailCache& fullResCache_;
    lrc::CatalogDatabase& catalog_;
    std::shared_ptr<lrc::ColorManager> colorManager_;
    std::unique_ptr<lrc::ExportEngine> exportEngine_;

    lrc::ImageBufferPtr linear_;
    qint64 loadedImageId_ = -1;
    qint64 latestGeneration_ = -1;
    std::atomic<bool> abort_{false};

    // PHASE_2 preview-resolution state.
    // Logical render size for the Fit/<=1:1 view (widget size x devicePixelRatio),
    // from ImageViewport::viewportResolutionChanged. Invalid QSize means "no
    // explicit target size" -> fall back to the historical render() path.
    QSize viewportSize_;
    // Native full-resolution long edge (px) of the current image, when known
    // (from a preview decode or a 1:1 decode). 0 = unknown -> renderPreview
    // falls back to source-relative sigma scaling.
    int fullLongEdge_ = 0;

    // Full-resolution 1:1 state, independent of the preview state above.
    lrc::ImageBufferPtr fullResLinear_;
    qint64 fullResImageId_ = -1;
    QString fullResPath_;
    qint64 fullResLatest_ = -1;
    uint64_t fullResAdjHash_ = 0;
    // 最近已发射 coarse 中间帧的 (image, adjHash),用于去重 (last (image, adjHash)
    // for which the 1:1 coarse intermediate frame was emitted; dedup guard).
    uint64_t fullResCoarseHash_ = 0;
    // 当前全尺寸画面来自磁盘缓存、尚无线性缓冲，adjustFullRes 需惰性解码 (current
    // full-res frame came from the disk cache with no linear buffer; adjustFullRes
    // must decode lazily).
    bool fullResFromCache_ = false;

    // M5-7: 最近非默认调整的全分辨率显示 U8 缓存 (recent full-res display U8
    // cache). Keyed by (imageId, adjHash); LRU with at most kFullResU8CacheSize
    // entries; default adjustments are never cached (the default 1:1 path reads
    // the embedded preview directly in loadFullRes). Every access happens on the
    // worker thread (loadFullRes/adjustFullRes), so no lock is required. Each
    // entry records the rendered buffer size so a native-size change invalidates
    // it even for the same imageId.
    struct FullResU8Entry {
        qint64 imageId = -1;
        uint64_t adjHash = 0;
        int width = 0;   // rendered full-res U8 width (native width).
        int height = 0;  // rendered full-res U8 height (native height).
        lrc::ImageBufferPtr u8;
    };
    static constexpr int kFullResU8CacheSize = 2;
    // index 0 = most recent, back = least recent (0 最近,尾部最久).
    std::vector<FullResU8Entry> fullResU8Cache_;

    // Returns the cached full-res U8 for (imageId, adjHash), or nullptr. When
    // `w`/`h` are > 0 they must match the cached buffer (native-size guard);
    // pass 0 when the native size is not yet known (no linear buffer decoded).
    lrc::ImageBufferPtr findFullResU8(qint64 imageId, uint64_t adjHash, int w,
                                      int h);
    // Inserts a freshly rendered full-res U8 into the LRU cache, evicting the
    // least-recently-used entry when at capacity. No-op for default adjustments.
    void storeFullResU8(qint64 imageId, uint64_t adjHash,
                        const lrc::ImageBufferPtr& u8);

    // --- M7: Before/After + Reference view ----------------------------------
    // Renders a linear source with the given adjustments at the current preview
    // resolution, mirroring loadImage/adjustPreview: renderPreview at
    // viewportSize_ when set (incremental per-stage cache), else render bounded
    // to maxDim. `fullLongEdge` is the source's native long edge for spatial-
    // kernel scaling (0 = source-relative). Returns null on abort or empty
    // output.
    lrc::ImageBufferPtr renderPreviewFrame(
        qint64 imageId, const lrc::ImageBuffer& linear,
        const lrc::Adjustments& adjustments, int maxDim, int fullLongEdge);
    // Resolves the catalog file path for an image id; empty when unknown. Used
    // by the before/after and reference requests to decode a photo the worker
    // has not loaded.
    QString pathForImage(qint64 imageId) const;
    // Independent generation guard for the reference-view path
    // (requestReference). Kept separate from latestGeneration_ so a reference
    // render neither depends on nor invalidates the working-image generation.
    qint64 refLatestGeneration_ = -1;
};
