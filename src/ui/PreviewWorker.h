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
#include "engine/SoftProofing.h"  // lrc::SoftProofParams for requestProof.
// The signals/slots below carry engine types (AdjustmentsPtr, Adjustments,
// ImageBufferPtr, HistogramBins, SoftProofParams): their metatypes must be
// declared before moc uses them (ui/MetaTypes.h is the single declaration
// point), and they cross thread boundaries as queued connections.
#include "ui/MetaTypes.h"

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
 *
 * `adjustments` is an immutable snapshot frozen when the item is built: the
 * producer copies the stack into a shared_ptr before the queued call, so the
 * QList<ExportItem> copy into the event carries only pointers and a later UI
 * edit cannot change what an in-flight batch exports. Null is rejected as a
 * per-item failure.
 */
struct ExportItem {
    qint64 imageId = 0;
    QString filePath;
    lrc::AdjustmentsPtr adjustments;
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
    // Adjustment-stack arguments below are lrc::AdjustmentsPtr (a shared_ptr to
    // a const Adjustments) instead of Adjustments by value: the UI publishes one
    // immutable snapshot per edit and every queued request derived from that
    // edit shares it, so the mask / curve / spot vectors are never deep-copied
    // into the event queue. Slots only read the pointee and never retain the
    // pointer past the call.
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
     * @param adjustments Shared immutable adjustment snapshot to render;
     *                    ignored (slot returns) when null.
     *
     * @note Keeps the decoded linear image as the working image for subsequent
     *       adjustPreview calls.
     */
    void loadImage(qint64 imageId, const QString& filePath, qint64 generation,
                   lrc::AdjustmentsPtr adjustments);
    /**
     * @brief Re-renders the working image with new adjustments (no raw decode).
     */
    void adjustPreview(qint64 imageId, qint64 generation,
                       lrc::AdjustmentsPtr adjustments);
    /**
     * @brief Coarse (low-res) preview re-render.
     *
     * Same semantics as adjustPreview but bounded to kCoarseMaxDim, giving a fast
     * low-res frame before the full preview lands (coarse-then-refine). Shares
     * the same latestGeneration_ guard, so stale coarse/refine renders are
     * dropped together.
     */
    void adjustPreviewCoarse(qint64 imageId, qint64 generation,
                             lrc::AdjustmentsPtr adjustments);
    /**
     * @brief Renders the current preview through the soft-proofing transform.
     *
     * `enabled == false` restores the unproofed preview by delegating to the
     * normal adjustPreview path, so toggling proofing off is lossless. When
     * enabled, the adjusted linear ProPhoto buffer is re-rendered and mapped
     * through SoftProofing::proofToDisplay, then emitted as proofReady.
     *
     * The destination field of `params` is either a built-in preset key
     * ("srgb" / "adobe_rgb" / "display_p3", serialized in memory) or a real
     * .icc/.icm file path. Keys this build cannot resolve (the CMYK proof
     * presets) emit proofFailed instead of silently showing the unproofed
     * frame. Runs on the worker thread; only the working image can be proofed
     * (a different image id is ignored like adjustPreview).
     */
    void requestProof(qint64 imageId, qint64 generation,
                      lrc::AdjustmentsPtr adjustments,
                      const lrc::SoftProofParams& params, bool enabled);
    /**
     * @brief Renders the out-of-gamut mask of the current preview.
     *
     * Produces a display-ready U8 frame (out-of-gamut pixels painted with the
     * requested alarm colour, everything else black) and emits
     * gamutReportReady; MainWindow writes it to the PNG the user picked.
     */
    void requestGamutReport(qint64 imageId,
                            lrc::AdjustmentsPtr adjustments,
                            const lrc::SoftProofParams& params);
    /**
     * @brief Arms soft proofing for the view's own render paths.
     *
     * requestProof() re-renders the current preview on demand. This slot instead
     * records the proof state so that every frame the viewport can receive is
     * produced proofed — the coarse drag frames (previewReadyCoarse), the refine
     * frame after each edit, and the 1:1 full-resolution frames
     * (fullResReady / fullResReadyCoarse). It also pushes the dialog's
     * out-of-gamut alarm colour into the engine, so the colour the user picks
     * tints the preview overlay and not just the exported gamut report.
     *
     * @param params  Destination profile key/path, rendering intent and alarm
     *                colour.
     * @param enabled False restores the unproofed look on the next frame (the
     *                recorded state is kept so re-enabling needs no re-send).
     *
     * @note Invoked from the UI thread via a queued connection; the recorded
     *       state is plain worker-thread data, so no extra locking is needed.
     *       A destination that cannot be resolved leaves proofing disarmed:
     *       frames fall back to the unproofed render and requestProof reports the
     *       failure to the user.
     */
    void setProofState(const lrc::SoftProofParams& params, bool enabled);
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
                     lrc::AdjustmentsPtr adjustments,
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
                             lrc::AdjustmentsPtr adjustments);
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
                     qint64 generation, lrc::AdjustmentsPtr adjustments);
    /**
     * @brief Re-renders the retained full-res linear buffer with new adjustments
     *        (no decode). No-op unless a full-res image is loaded.
     */
    void adjustFullRes(qint64 imageId, qint64 generation,
                       lrc::AdjustmentsPtr adjustments);
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
                             lrc::AdjustmentsPtr adjustments);
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
                            lrc::AdjustmentsPtr afterAdj,
                            lrc::AdjustmentsPtr beforeAdj,
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
                          lrc::AdjustmentsPtr refAdj,
                          int maxDim = lrc::app::kPreviewMaxDim);

signals:
    /**
     * @brief Display-ready sRGB preview (RGB U8) for a photo.
     * @param imageId Catalog id the preview belongs to.
     * @param rgbU8   Rendered preview buffer, or null on failure.
     */
    void previewReady(qint64 imageId, const lrc::ImageBufferPtr& rgbU8);
    /**
     * @brief Wall-clock cost of the refined frame just emitted.
     *
     * Measured on the worker thread from the moment the request is dequeued
     * until previewReady is emitted for that frame (the A2 metric). Coarse
     * drag frames and image loads deliberately do not emit it. Values are
     * milliseconds and always >= 0.
     */
    void previewTimingReady(qint64 imageId, double ms);
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
     * @brief Display-ready sRGB frame produced by the soft-proofing transform.
     *
     * Same shape as previewReady; emitted by requestProof when proofing is
     * enabled. MainWindow swaps it into the viewport without resetting the
     * view (the frame always belongs to the already-visible image).
     */
    void proofReady(qint64 imageId, const lrc::ImageBufferPtr& rgbU8);
    /**
     * @brief Soft proofing could not be applied (unresolvable destination
     *        profile, no proof transform, or no working image).
     */
    void proofFailed(const QString& message);
    /**
     * @brief Out-of-gamut report frame for requestGamutReport (U8 RGB).
     */
    void gamutReportReady(const lrc::ImageBufferPtr& rgbU8);
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
    /**
     * @brief Content-aware crop suggestion from the last B-1 auto-develop run.
     *
     * The rectangle is normalized to [0,1] over the source image and drawn as a
     * pure-guidance box by the viewport. It is never applied to pixels (the
     * pipeline has no crop-rendering stage yet). hasCrop == false means "no crop
     * recommended" and the rect values are undefined.
     *
     * @param hasCrop  Whether a crop box was suggested.
     * @param left     Normalized left edge in [0,1].
     * @param top      Normalized top edge in [0,1].
     * @param right    Normalized right edge in [0,1].
     * @param bottom   Normalized bottom edge in [0,1].
     */
    void autoCropSuggestionReady(bool hasCrop, float left, float top,
                                 float right, float bottom);
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

    // --- Soft proofing (P-C "视图 → 软打样…") ---------------------------------
    // Proof transform cache. Used exclusively on the worker thread, so the
    // engine's own locks are only there to make the shared-mutex API safe, not
    // to guard additional state here.
    lrc::SoftProofing proofing_;
    // Destination key/path currently loaded into proofing_ (empty = none). Lets
    // repeated parameter tweaks skip the (re)load + transform rebuild.
    std::string proofProfileKey_;

    /**
     * @brief Ensures `destination` is the profile loaded into proofing_.
     *
     * Built-in preset keys are serialized in memory through
     * ColorManager::exportProfileBytes; anything else is treated as an
     * .icc/.icm path and loaded from disk. Returns false (and emits nothing)
     * when the destination cannot be resolved, leaving proofProfileKey_ empty
     * so a later request retries.
     */
    bool ensureProofProfileLoaded(const std::string& destination);

    /**
     * @brief Records the proof state and syncs the engine with it.
     *
     * Stores @p params / @p enabled for the render paths, pushes the alarm colour
     * into `proofing_` and resolves the destination profile.
     *
     * @return True when the state is armed *and* usable (see proofArmed()).
     */
    bool applyProofState(const lrc::SoftProofParams& params, bool enabled);

    /**
     * @brief True when the render paths must proof their output.
     *
     * Mirrors the condition requestProof() checks before proofing: proofing
     * switched on, a destination key/path recorded, and its profile loaded.
     */
    bool proofArmed() const {
        return proofEnabled_ && !proofParams_.destinationProfilePath.empty() &&
               proofing_.hasProofProfile();
    }

    /**
     * @brief Proofs an adjusted linear buffer with the recorded state.
     *
     * @param linearDisplay Adjusted linear ProPhoto F32 buffer — the same stage
     *        the unproofed path converts with ColorManager::toDisplaySRGB.
     * @return The proofed display-ready U8 frame, or nullptr when proofing is not
     *         armed / unusable, so callers keep their unproofed frame.
     */
    lrc::ImageBufferPtr proofLinear(const lrc::ImageBufferPtr& linearDisplay);

    /**
     * @brief Salt mixed into the 1:1 frame keys to reflect the proof state.
     *
     * The full-res frame dedup keys are (image, adjustments). Proofing changes
     * the pixels for the same adjustment stack, so switching it on/off, changing
     * the intent or picking another alarm colour must invalidate those keys —
     * otherwise the viewport would keep an unproofed frame (or vice versa).
     *
     * @return 0 when proofing is not armed (keys reduce to the plain adjustment
     *         hash, i.e. the historical behaviour).
     */
    uint64_t proofFrameSalt() const;

    // Last state pushed by setProofState / requestProof: the render paths consult
    // it so coarse, refine and 1:1 frames all come out proofed.
    bool proofEnabled_ = false;
    lrc::SoftProofParams proofParams_;

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
     *
     * @param renderMs Worker-thread wall clock measured from the request being
     *                 dequeued to this frame, in milliseconds. Emitted via
     *                 previewTimingReady right after previewReady. Pass a
     *                 negative value for paths that are not part of the A2
     *                 refined-frame metric (load and coarse frames).
     */
    void emitPreviewWithHistogram(qint64 imageId, qint64 generation,
                                  const lrc::ImageBufferPtr& linearDisplay,
                                  const lrc::ImageBufferPtr& rgbU8,
                                  double renderMs = -1.0);
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
