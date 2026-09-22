#pragma once

/**
 * @brief Background preview, export and full-res worker for the UI.
 *
 * Owns the current decoded linear image so repeated slider changes re-render
 * without touching the raw file again. Lives on its own QThread; results cross
 * back to the UI thread via queued signals, and the UI issues requests through
 * queued slots/connections.
 */
#include <atomic>
#include <optional>
#include <vector>

#include <QImage>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include "app/Constants.h"
#include "engine/Adjustments.h"
#include "engine/DcpProfile.h"
#include "engine/ImageBuffer.h"
#include "engine/Pipeline.h"  // lrc::HistogramBins for the histogramReady signal.

/**
 * Export formats understood by PreviewWorker::exportImage. Index order must
 * match the ExportDialog combo.
 */
enum class ExportFormat : int {
    Jpeg = 0,
    Png,
    Tiff8,
    Tiff16,
    Dng,
};

/**
 * @brief Watermark options that cross the queued connection to PreviewWorker.
 *
 * Mirrors ExportEngine::ExportSpec::Watermark using Qt container types so it is
 * a registered metatype (the QVector holds straight-alpha RGBA pixels).
 */
struct ExportWatermark {
    bool enabled = false;
    QVector<uint8_t> rgba;    // overlayW*overlayH*4 straight-alpha RGBA
    int overlayW = 0;
    int overlayH = 0;
    int position = 4;         // 0..3 corners, 4 = centre
    float opacity = 1.0f;     // 0..1
    float scale = 0.2f;       // overlay long edge / output long edge
};
Q_DECLARE_METATYPE(ExportWatermark)

/**
 * @brief One entry in a batch export.
 *
 * `maxDim` is the longest-edge cap in pixels (0 = original / native
 * resolution). `quality` is the JPEG quality (0..100, ignored for lossless
 * formats). `icc` is an ExportEngine::ExportSpec::IccChoice value;
 * `watermark` is optional. Crosses the queued connection from MainWindow to
 * PreviewWorker::batchExport; registered in main().
 */
struct ExportItem {
    qint64 imageId = 0;
    QString filePath;
    lrc::Adjustments adjustments;
    QString outPath;
    ExportFormat format = ExportFormat::Jpeg;
    int quality = 92;
    int maxDim = 0;
    int icc = 0;                  // IccChoice
    ExportWatermark watermark;
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

    /**
     * @brief Requests worker shutdown.
     *
     * Long-running slots check and bail at entry. Thread-safe; called from the
     * UI thread.
     */
    void requestAbort() { abort_.store(true); }

public slots:
    /**
     * @brief Sets the viewport's logical render size for Fit/<=1:1 views.
     *
     * Value is the widget size times devicePixelRatio. Stored and applied by the
     * next loadImage/adjustPreview; delivered via a queued connection on the
     * worker thread, so no lock is needed.
     */
    void setPreviewTargetSize(const QSize& size);
    /**
     * @brief Updates the active DCP camera colour profile for subsequent decodes.
     *
     * Payload is the absolute path to a .dcp file; an empty string clears the
     * selection. Because this slot joins the worker thread via a queued
     * connection, dcp_ is mutated on the same thread that runs loadImage /
     * decodeFullRes / exportImage, so no lock is needed. Changing the profile does
     * NOT re-decode automatically -- the UI must issue a fresh loadImage so the
     * new matrix/curve reaches the pipeline.
     */
    void setDcpProfile(const QString& path);
    /**
     * @brief Decodes a raw at preview resolution and renders it with adjustments.
     *
     * @param imageId    Catalog id of the photo to load.
     * @param filePath   Path of the raw file to decode.
     * @param generation Generation token; stale requests are dropped.
     * @param adjustments Adjustment stack to render.
     *
     * @note Keeps the decoded linear image as the working image for subsequent
     *       adjustPreview calls.
     */
    void loadImage(qint64 imageId, const QString& filePath,
                   qint64 generation, const lrc::Adjustments& adjustments);
    /**
     * @brief Re-renders the working image with new adjustments (no raw decode).
     */
    void adjustPreview(qint64 imageId, qint64 generation,
                       const lrc::Adjustments& adjustments);
    /**
     * @brief Coarse (low-res) preview re-render.
     *
     * Same semantics as adjustPreview but bounded to kCoarseMaxDim, giving a fast
     * low-res frame before the full preview lands (coarse-then-refine). Shares
     * the same latestGeneration_ guard, so stale coarse/refine renders are
     * dropped together.
     */
    void adjustPreviewCoarse(qint64 imageId, qint64 generation,
                             const lrc::Adjustments& adjustments);
    /**
     * @brief Full-resolution decode + render + write to an image file.
     *
     * @param format  An ExportFormat value; @p quality is the JPEG quality
     *                (0..100, ignored for lossless formats).
     * @param maxDim  Longest-edge cap; 0 keeps native resolution.
     * @param icc     ExportEngine::ExportSpec::IccChoice value.
     * @param watermark Optional watermark overlay.
     *
     * @return Emits exportFinished on completion.
     */
    void exportImage(qint64 imageId, const QString& filePath,
                     const lrc::Adjustments& adjustments,
                     const QString& outPath, int format, int quality,
                     int maxDim = 0, int icc = 0,
                     const ExportWatermark& watermark = ExportWatermark{});
    /**
     * @brief Serial batch export.
     *
     * Each item is decoded + rendered + written in turn; the export bottleneck is
     * encode, not decode. Emits batchExportProgress per item and a single
     * batchExportFinished at the end with aggregate success/fail counts. Aborts
     * mid-batch when requestAbort() was called; the counts then reflect only the
     * items actually attempted.
     */
    void batchExport(const QList<ExportItem>& items);
    /**
     * @brief Computes the auto-adjust suggestion for the current photo.
     *
     * Decodes (or reuses the cached linear image) the photo.
     */
    void computeAutoAdjust(qint64 imageId, const QString& filePath);
    /**
     * @brief Renders an Alt-key diagnostic overlay from the working linear image.
     *
     * @param mode Pipeline::VisualizeMode as an int.
     */
    void renderVisualization(qint64 imageId, qint64 generation, int mode,
                             const lrc::Adjustments& adjustments);
    /**
     * @brief Decodes a bracketed sequence, merges to a linear HDR buffer and
     *        writes a linear DNG.
     *
     * On success hdrFinished carries the output path so MainWindow can import the
     * merged file back into the catalog.
     */
    void mergeHdr(const QStringList& paths, bool deghost,
                  const QString& outPath);
    /**
     * @brief Converts a raw to a mosaic-preserving raw DNG.
     *
     * On success convertFinished carries the output path so MainWindow can import
     * the DNG back into the catalog.
     */
    void convertToDng(qint64 imageId, const QString& filePath,
                      const QString& outPath);
    /**
     * @brief Decodes at full resolution and retains the full-res linear buffer.
     *
     * Decodes with DecodeParams{} (halfSize=false, maxDim=0), renders to display
     * sRGB and retains the full-res linear buffer as the working image. Emits
     * fullResReady when this generation is still current.
     */
    void loadFullRes(qint64 imageId, const QString& filePath,
                     qint64 generation, const lrc::Adjustments& adjustments);
    /**
     * @brief Re-renders the retained full-res linear buffer with new adjustments
     *        (no decode). No-op unless a full-res image is loaded.
     */
    void adjustFullRes(qint64 imageId, qint64 generation,
                       const lrc::Adjustments& adjustments);
    /**
     * @brief Coarse 1:1 intermediate re-render, bounded to kCoarseMaxDim.
     *
     * Renders from the retained full-res linear buffer without touching the raw
     * file again. Emits fullResReadyCoarse when this generation is current.
     * MainWindow throttles this during 1:1 drags. adjustFullRes ships its own
     * coarse frame first, so leaving this slot unwired degrades safely to
     * coarse-then-refine on the refine path.
     */
    void adjustFullResCoarse(qint64 imageId, qint64 generation,
                             const lrc::Adjustments& adjustments);
    /**
     * @brief Releases the retained full-res buffer(s). Called on exit 1:1 /
     *        image switch.
     */
    void clearFullRes();

    /**
     * @brief Renders both the After (current) and Before (default snapshot)
     *        states of the working image, then emits beforeAfterReady.
     *
     * The two passes reuse the loaded preview-resolution linear buffer and the
     * pipeline caches. Joins the same generation guard as loadImage/adjustPreview:
     * a newer request supersedes it, while the internal After/Before pair share
     * one generation and never invalidate each other. When After and Before are
     * the same adjustment stack, the single render is reused for both.
     */
    void requestBeforeAfter(qint64 imageId, qint64 generation,
                            const lrc::Adjustments& afterAdj,
                            const lrc::Adjustments& beforeAdj,
                            int maxDim = lrc::app::kPreviewMaxDim,
                            bool halfSize = true);
    /**
     * @brief Renders an arbitrary library photo at preview resolution.
     *
     * Uses the photo's own adjustment stack and emits referenceReady. Prefers the
     * import-time standard preview when available (no raw decode). Runs on an
     * independent generation guard (refLatestGeneration_) and never touches the
     * working image's linear_/latestGeneration_, so a reference render cannot
     * collide with the main preview path.
     */
    void requestReference(qint64 refImageId, qint64 generation,
                          const lrc::Adjustments& refAdj,
                          int maxDim = lrc::app::kPreviewMaxDim);

signals:
    /**
     * @brief Display-ready sRGB preview (RGB U8) for a photo.
     * @param imageId Catalog id the preview belongs to.
     * @param rgbU8   Rendered preview buffer, or null on failure.
     */
    void previewReady(qint64 imageId, const lrc::ImageBufferPtr& rgbU8);
    /**
     * @brief Display-encoded RGB histogram for the preview just emitted.
     *
     * Emitted right after previewReady, computed on the worker thread from the
     * adjusted linear F32 display buffer (full traversal, no sampling) so the
     * UI never blocks on histogram statistics. Falls back to the U8 frame when
     * the incremental cache served the preview unchanged.
     */
    void histogramReady(qint64 imageId, const lrc::HistogramBins& bins);
    /**
     * @brief Low-res (kCoarseMaxDim) preview frame, delivered ahead of
     *        previewReady for fast drag feedback.
     */
    void previewReadyCoarse(qint64 imageId, const lrc::ImageBufferPtr& rgbU8);
    /**
     * @brief Native pixel dimensions of the decoded image (before any preview
     *        downscale). Emitted once per successful decode; lets the 1:1
     *        viewport map the downscaled preview at true full size while
     *        full-res loads.
     */
    void imageMeta(qint64 imageId, int nativeWidth, int nativeHeight);
    void cameraMeta(qint64 imageId, const QString& make, const QString& model,
                    const QString& lensMaker, const QString& lensModel,
                    float focalLengthMm, float aperture, float cropFactor);
    /**
     * @brief The raw could not be loaded or rendered.
     */
    void loadFailed(const QString& filePath);
    void exportFinished(bool ok, const QString& message);
    /**
     * @brief Batch export progress. @p done is 1-based, @p total == items.size().
     */
    void batchExportProgress(int done, int total);
    /**
     * @brief Aggregate result of a batch export. @p lastError carries the last
     *        error message when any item failed (empty on full success).
     */
    void batchExportFinished(int successCount, int failCount,
                             const QString& lastError);
    void autoAdjustReady(const lrc::Adjustments& adjustments);
    void visualizationReady(qint64 imageId, qint64 generation,
                            const lrc::ImageBufferPtr& overlay);
    void hdrProgress(int done, int total);
    void hdrFinished(bool ok, const QString& message, const QString& outPath);
    void convertFinished(bool ok, const QString& message, const QString& outPath);
    /**
     * @brief Full-resolution display-ready sRGB buffer (RGB U8). Same shape as
     *        previewReady -- ImageBufferPtr is already a registered metatype.
     */
    void fullResReady(qint64 imageId, const lrc::ImageBufferPtr& rgbU8);
    /**
     * @brief Low-res (kCoarseMaxDim) 1:1 frame delivered ahead of fullResReady
     *        so the full-res viewport gets immediate drag feedback while the
     *        full-res render runs (coarse-then-refine).
     */
    void fullResReadyCoarse(qint64 imageId, const lrc::ImageBufferPtr& rgbU8);
    /**
     * @brief Both Before and After previews for one Before/After view are ready.
     *
     * Both are display-ready sRGB QImages at the current preview resolution.
     * When the Before and After adjustment stacks are identical, @p before is
     * the same buffer as @p after.
     */
    void beforeAfterReady(qint64 imageId, const QImage& before,
                          const QImage& after);
    /**
     * @brief The reference photo's preview is ready. A null QImage signals that
     *        the reference could not be produced (image not in the catalog, or a
     *        decode/render failure).
     */
    void referenceReady(qint64 refImageId, const QImage& image);

private:
    /**
     * @brief Decodes the raw at full resolution into fullResLinear_ (shared by
     *        loadFullRes and adjustFullRes' lazy decode).
     * @return False on decode failure.
     */
    bool decodeFullRes(qint64 imageId, const QString& filePath);
    /**
     * @brief Renders the 1:1 coarse intermediate (kCoarseMaxDim) from
     *        fullResLinear_ and emits fullResReadyCoarse.
     *
     * @note Dedups on fullResCoarseHash_ and never touches fullResAdjHash_ (the
     *       full-res frame state), so the coarse frame cannot suppress the
     *       subsequent full render.
     */
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
    /**
     * Active DCP camera colour profile, applied at decode time. Empty when the
     * user has not selected a profile (or selected "none"). See setDcpProfile.
     */
    std::optional<lrc::DcpProfile> dcp_;

    // Preview-resolution state.
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
    // Last (image, adjHash) for which the 1:1 coarse intermediate frame was
    // emitted; dedup guard.
    uint64_t fullResCoarseHash_ = 0;
    // Current full-res frame came from the disk cache with no linear buffer;
    // adjustFullRes must decode lazily.
    bool fullResFromCache_ = false;

    /**
     * @brief Recent full-res display U8 cache (by (imageId, adjHash)).
     *
     * LRU with at most kFullResU8CacheSize entries; default adjustments are never
     * cached (the default 1:1 path reads the embedded preview directly in
     * loadFullRes). Every access happens on the worker thread
     * (loadFullRes/adjustFullRes), so no lock is required. Each entry records the
     * rendered buffer size so a native-size change invalidates it even for the
     * same imageId.
     */
    struct FullResU8Entry {
        qint64 imageId = -1;
        uint64_t adjHash = 0;
        int width = 0;   // rendered full-res U8 width (native width).
        int height = 0;  // rendered full-res U8 height (native height).
        lrc::ImageBufferPtr u8;
    };
    static constexpr int kFullResU8CacheSize = 2;
    // index 0 = most recent, back = least recent (0 most recent, tail oldest).
    std::vector<FullResU8Entry> fullResU8Cache_;

    /**
     * @brief Returns the cached full-res U8 for (imageId, adjHash), or nullptr.
     *
     * When `w`/`h` are > 0 they must match the cached buffer (native-size guard);
     * pass 0 when the native size is not yet known (no linear buffer decoded).
     */
    lrc::ImageBufferPtr findFullResU8(qint64 imageId, uint64_t adjHash, int w,
                                      int h);
    /**
     * @brief Inserts a freshly rendered full-res U8 into the LRU cache, evicting
     *        the LRU entry when at capacity. No-op for default adjustments.
     */
    void storeFullResU8(qint64 imageId, uint64_t adjHash,
                        const lrc::ImageBufferPtr& u8);

    /**
     * @brief Renders a linear source with the given adjustments at the current
     *        preview resolution, mirroring loadImage/adjustPreview.
     *
     * Uses viewportSize_ when set (incremental per-stage cache), else renders
     * bounded to maxDim. @p fullLongEdge is the source's native long edge for
     * spatial-kernel scaling (0 = source-relative).
     * @return The rendered display buffer, or nullptr on abort or empty output.
     */
    lrc::ImageBufferPtr renderPreviewFrame(
        qint64 imageId, const lrc::ImageBuffer& linear,
        const lrc::Adjustments& adjustments, int maxDim, int fullLongEdge);
    /**
     * @brief Emits previewReady then histogramReady for one rendered frame.
     *
     * The histogram is computed here (worker thread) from the adjusted linear
     * F32 display buffer -- a full traversal with no sampling, so the UI thread
     * never runs the statistics. When @p linearDisplay is null/empty (the
     * incremental cache served the unchanged preview), the U8 frame itself is
     * used instead. Both signals are guarded by the generation token.
     */
    void emitPreviewWithHistogram(qint64 imageId, qint64 generation,
                                  const lrc::ImageBufferPtr& linearDisplay,
                                  const lrc::ImageBufferPtr& rgbU8);
    /**
     * @brief Resolves the catalog file path for an image id. Empty when unknown.
     *
     * @note Used by the before/after and reference requests to decode a photo the
     *       worker has not loaded.
     */
    QString pathForImage(qint64 imageId) const;
    // Independent generation guard for the reference-view path
    // (requestReference). Kept separate from latestGeneration_ so a reference
    // render neither depends on nor invalidates the working-image generation.
    qint64 refLatestGeneration_ = -1;
};
