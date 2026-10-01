#pragma once

/**
 * @brief Top-level MainWindow with a three-pane layout (navigation /
 *        preview+filmstrip / develop), menu bar, status bar, and all background
 *        workers. Lives on the UI (main) thread; the UI layer talks to the
 *        engine and catalog exclusively through signals/slots, delegating heavy
 *        work to child workers on their own QThreads.
 */

#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <QColor>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QMainWindow>
#include <QSet>
#include <QSplitter>
#include <QStringList>
#include <QVector>

#include "app/DuplicateDetector.h"
#include "catalog/CatalogDatabase.h"
#include "catalog/LibraryManager.h"
#include "engine/Adjustments.h"
#include "engine/ImageBuffer.h"
#include "engine/Pipeline.h"  // lrc::HistogramBins for the histogramReady slot.
#include "engine/RawDecoder.h"
#include "engine/SoftProofing.h"  // lrc::SoftProofParams for the proof slots.
#include "ui/FilmstripWidget.h"
#include "ui/ImageViewport.h"
// The slots below carry engine types (Adjustments, ImageBufferPtr, HistogramBins,
// SoftProofParams): their metatypes must be declared before moc uses them.
#include "ui/MetaTypes.h"
#include "utils/UndoStack.h"

namespace lrc {

class ColorManager;
class AiInferenceEngine;       // ONNX Runtime wrapper
class ModelManager;            // manifest + download/cache lifecycle
class FolderMonitor;
class Pipeline;
class PresetLibrary;           // named preset store (P3-3)
class ThumbnailCache;
class SoftProofingDialog;      // modeless proofing dialog (视图 → 软打样…)
} // namespace lrc

class CollectionsDialog;
class DevelopPanel;
class ImportWorker;
class ImageViewport;
class NavigatorWidget;
class PreviewWorker;
class QAction;
class QButtonGroup;
class QCloseEvent;
class QLineEdit;
class QScrollArea;
class QThread;
class QTimer;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    /**
     * @brief Flushes the debounced adjustment save before the window disappears.
     */
    void closeEvent(QCloseEvent* event) override;

signals:
    /**
     * @brief The main thread hands one lazy filmstrip thumbnail to the
     *        background decoder (queued connection to a hub living on
     *        thumbLoaderThread_). Kept in the header so AUTOMOC knows the
     *        signal.
     */
    void thumbDecodeRequested(int imageId, const QString& thumbnailPath);

private slots:
    void importFolder();
    void importFiles();
    void newCatalog();
    void openCatalog();
    void backupCatalog();
    void optimizeCatalog();
    /**
     * @brief Background backup completion (invoked back on the UI thread).
     */
    void onCatalogBackupFinished(bool ok, const QString& error,
                                 const QString& destPath);
    /**
     * @brief Background optimize completion (invoked back on the UI thread).
     */
    void onCatalogOptimizeFinished(bool ok, const QString& error);
    void showEditHistory();
    void exportImage();
    void mergeHdr();
    void onSetRating(int imageId, int rating);
    void onSetFlag(int imageId, int flag);
    void onSetLabelColor(int imageId, int color);
    void onCollectionToggle(int imageId, int collectionId, bool add);
    void onCollectionSelected(int collectionId);
    void onManageCollections();
    void onRemoveImage(int imageId);
    /**
     * @brief Opens the snapshot panel for the current image.
     */
    void onManageSnapshots();
    void onFiltersChanged(int minRating, int flagFilter, int labelColorFilter);
    void onImageSelected(int imageId);
    void onAdjustmentsChanged(const lrc::Adjustments& adjustments);
    /**
     * @brief Coarse-then-refine: slider drag (30 ms throttle) drives a low-res
     *        preview; onAdjustmentsChanged (refine) still fires on pause/release.
     */
    void onAdjustmentsChangedCoarse(const lrc::Adjustments& adjustments);
    void onAdjustmentsCommitted(const lrc::Adjustments& adjustments);
    void onAutoRequested();
    void onAutoAdjustReady(const lrc::Adjustments& adjustments);
    void onAutoCropSuggestionReady(bool hasCrop, float left, float top,
                                   float right, float bottom);
    void onColorSamplerRequested(bool active);
    void onColorSampled(int colorRangeIndex, const QColor& color);
    void onColorAdjusted(int colorRangeIndex, double saturation,
                         double luminance);
    void onColorAdjustedFinished();
    void onColorTargetSamplerRequested(bool active);
    void onPointColorSampled(const QColor& color);
    void undoAdjustment();
    void redoAdjustment();
    void onMaskGeometryChanged(const lrc::Mask& mask);
    void onMaskEditFinished();
    void onMaskPromptClearRequested(int index);
    void onZoomModeChanged(ImageViewport::ZoomMode mode);
    void onMaskToolActivated(lrc::MaskType tool);
    void onWhiteBalanceSampled(const QColor& color);
    void onColorRangeSampled(const QColor& color);
    void onEditGestureStarted();
    void onEditGestureFinished();
    void onVisualizationRequested(int mode,
                                  const lrc::Adjustments& adjustments);
    void onVisualizationCleared();
    void onVisualizationReady(qint64 imageId, qint64 generation,
                              const lrc::ImageBufferPtr& overlay);
    void onPreviewReady(qint64 imageId, const lrc::ImageBufferPtr& rgbU8);
    void onPreviewReadyCoarse(qint64 imageId,
                              const lrc::ImageBufferPtr& rgbU8);
    /**
     * @brief P3-1: applies the worker-computed linear-light RGB histogram.
     *
     * Arrives right after onPreviewReady (worker emits histogramReady second),
     * so the histogram always reflects the adjusted linear display buffer.
     */
    void onHistogramReady(qint64 imageId, const lrc::HistogramBins& bins);
    void onFullResRequested(bool needed);
    void onImageMeta(qint64 imageId, int w, int h);
    void onCameraMeta(qint64 imageId, const QString& make,
                      const QString& model, const QString& lensMaker,
                      const QString& lensModel, float focalLengthMm,
                      float aperture, float cropFactor);
    void onDcpProfileSelected(const QString& path);
    // The develop panel's Lensfun enable checkbox was toggled. The adjustment
    // stack is persisted by onAdjustmentsCommitted; this slot only surfaces
    // user feedback so the toggle is not silent.
    void onLensfunEnabledChanged(bool enabled);

    // --- Soft proofing (视图 → 软打样…) --------------------------------------
    // Opens (or re-raises) the modeless soft-proofing dialog.
    void openSoftProofing();
    // The dialog's parameters changed: re-render the current preview through
    // SoftProofing. `enabled == false` restores the unproofed frame.
    void onSoftProofParamsChanged(const lrc::SoftProofParams& params,
                                  bool enabled);
    // Proofed frame from the preview worker (swapped into the viewport without
    // resetting zoom/pan).
    void onProofReady(qint64 imageId, const lrc::ImageBufferPtr& rgbU8);
    // Soft proofing could not run; the message says why.
    void onProofFailed(const QString& message);
    // The dialog asked for an out-of-gamut report PNG.
    void onExportGamutReport();
    // The worker finished the gamut mask; write it to the path the user picked.
    void onGamutReportReady(const lrc::ImageBufferPtr& rgbU8);

    // --- Duplicate detection (照片 → 查找重复项…) ----------------------------
    // Starts a background scan (one at a time) over the catalog's cached
    // thumbnails and shows DuplicateResultsDialog when it finishes.
    void onFindDuplicates();
    void onDuplicateScanFinished();
    // The dialog asked to focus one or more members; jump to the first one that
    // is present in the current filmstrip listing.
    void onDuplicateFocusImages(const QVector<int64_t>& imageIds);
    void openMaskWindow();
    void onFullResReady(qint64 imageId, const lrc::ImageBufferPtr& rgbU8);
    /**
     * @brief Low-res (kCoarseMaxDim) 1:1 intermediate frame, delivered ahead of
     *        fullResReady so a 1:1 slider drag gets immediate coarse feedback.
     */
    void onFullResReadyCoarse(qint64 imageId,
                              const lrc::ImageBufferPtr& rgbU8);
    /**
     * @brief Thumbnail decoded on the import worker thread, delivered as a QImage.
     */
    void onThumbReadyImage(int imageId, const QImage& thumb);
    void onThumbReady(int imageId, const QString& thumbnailPath);
    /**
     * @brief The filmstrip asked for a lazy thumbnail (a visible row has none
     *        yet); enqueue it for background decode (bounded queue, one job in
     *        flight).
     */
    void onThumbnailRequested(int imageId);
    /**
     * @brief A background decode finished -- dispatch the next queued request.
     */
    void onThumbnailDecoded(int imageId);
    /**
     * @brief Trailing edge of a histogram/status-bar throttle window.
     */
    void flushHistogramThrottle();
    /**
     * @brief Adjustment-save debounce window elapsed -> persist latest value.
     */
    void onSaveDebounceTimeout();
    void onImportProgress(int done, int total);
    void onImportFinished(int addedCount);
    void onImportWarning(const QString& message);
    /**
     * @brief A B-2 pregrade marker arrived for `imageId`; update the filmstrip
     *        badge for every row sharing that image id.
     */
    void onPregradeReady(int imageId, bool hasSubject, int score,
                         bool suggestAuto);
    void onExportFinished(bool ok, const QString& message);
    void onHdrProgress(int done, int total);
    void onHdrFinished(bool ok, const QString& message, const QString& outPath);
    void onConvertFinished(bool ok, const QString& message, const QString& outPath);
    void importPreset();
    void exportPreset();
    /**
     * @brief Opens the named-preset manager (save / apply / delete / export).
     */
    void onManagePresets();
    void watchFolderPoll();

    // --- Spot removal ------------------------------------------------------
    // The viewport's spot list changed (create / move / delete / drag). Apply it
    // to the current Adjustments, re-render live, and commit once the gesture
    // idles (see onSpotCommitTimeout).
    void onSpotRemovalsEdited(const std::vector<lrc::SpotRemoval>& spots);
    // The viewport's spot tool armed/disarmed -> keep the toolbar button in sync.
    void onSpotToolActiveChanged(bool active);
    // The spot gesture is idle -> push one undo entry and persist.
    void onSpotCommitTimeout();

    // --- Virtual copies ------------------------------------------------------
    void createVirtualCopy();
    // A filmstrip virtual-copy row was selected (variantId != 0). Master rows are
    // still served by onImageSelected.
    void onImageVariantSelected(int64_t imageId, int variantId);
    // A Navigator virtual-copy item was clicked (image + variant).
    void onVirtualCopySelected(int64_t imageId, int variantId);

    // --- Before/after + reference view ---------------------------------------
    // Toolbar mirror + (re)render request for a non-Off compare mode.
    void onCompareModeChanged(ImageViewport::CompareMode mode);
    void onBeforeAfterReady(qint64 imageId, const QImage& before,
                            const QImage& after);
    void onReferenceReady(qint64 refImageId, const QImage& image);
    void toggleReferenceView();

    // --- Keywords & metadata --------------------------------------------------
    void onKeywordSelected(int64_t keywordId, const QString& fullPath);
    void onMetadataEdited(const std::string& title, const std::string& caption,
                          const std::string& copyright,
                          const std::string& creator,
                          const std::vector<std::string>& keywords);
    void applySearchFilter();

    // --- Smart collections ----------------------------------------------------
    void onSmartCollectionSelected(int64_t smartCollectionId);
    void onNewSmartCollectionRequested();

    // --- Panorama --------------------------------------------------------------
    void mergePanorama();
    // PanoramaDialog no longer carries PanoramaOptions across the signal
    // (Qt 6.8 metatype conflict); the applied options are read from the dialog
    // via options() after exec() when needed.
    void onPanoramaMerged(const lrc::ImageBufferPtr& result);

private:
    void buildUi();
    void buildToolbar();
    void buildMenus();
    void startWorkers();
    void stopWorkers();
    // Write the window geometry and pane proportions to QSettings. Idempotent,
    // and safe to call when the window has never been shown.
    void saveUiState();
    // Restore what saveUiState() wrote. Silently keeps the built-in defaults
    // (resize(1320, 820) and the splitter sizes set in buildUi) on a first run
    // or whenever a stored value is missing or unusable.
    void restoreUiState();
    // Request abort + quit the event loops of every background worker.
    void requestWorkersStop();
    // True once no worker thread is still running.
    bool allWorkersStopped() const;
    // Join the catalog backup/optimize thread (must finish before the catalog_
    // member is destroyed).
    void joinDbOpThread();
    // Immediate histogram + status-bar refresh (final/refine frames).
    void updateHistogramNow(const QImage& image);
    // Throttled histogram + status-bar refresh for coarse frames.
    void requestHistogramUpdate(const QImage& image);
    // Persist the debounced adjustment (pendingSave_) immediately.
    void flushPendingSave();
    // Discard a queued adjustment save (used by undo/redo).
    void cancelPendingSave();
    void refreshFilmstrip();
    // Start the next queued lazy-thumbnail decode if one is not running.
    void maybeDispatchThumbDecode();
    void refreshLibraryFolders();
    void startImport(const QStringList& paths, bool convertToDng = false,
                     lrc::ImportMode mode = lrc::ImportMode::Add,
                     const QString& destRoot = QString(),
                     const QString& renameTemplate = QString());
    void convertToDng();
    void requestPreview(const QString& filePath, const lrc::Adjustments& adjustments);
    void requestAdjust(const lrc::Adjustments& adjustments);
    // Low-res (kCoarseMaxDim) re-render of the working image; mirrors
    // requestAdjust but targets PreviewWorker::adjustPreviewCoarse.
    void requestAdjustCoarse(const lrc::Adjustments& adjustments);
    void requestFullRes();
    void clearFullRes();
    void requestAdjustFullRes();
    // 1:1 coarse-then-refine -- fast low-res re-render of the retained full-res
    // buffer while a slider is dragged in 1:1 (mirrors requestAdjustFullRes but
    // targets PreviewWorker::adjustFullResCoarse).
    void requestAdjustFullResCoarse(const lrc::Adjustments& adjustments);
    void updateHistogramAndStatus(const QImage& image);
    void setFilterFolder(const QString& folder);
    QImage toQImage(const lrc::ImageBufferPtr& buffer) const;

    // --- Integration helpers ---------------------------------------------------
    // Shared image-load path: master (variantId 0) and virtual copies both route
    // here (the old onImageSelected body, parameterized by variant).
    void loadImageState(int imageId, int variantId);
    // Pushes the current Adjustments' spot list + count into the viewport/panel.
    void syncSpotStateFromAdjustments();
    // Rebuilds the Navigator "virtual copy" group from the full catalog listing.
    void buildNavigatorVirtualCopies();
    // Clears every navigator-driven filter dimension (folder / collection /
    // keyword / smart collection / search). Used when entering a new filter.
    void clearImageFilters();
    // Full keyword paths assigned to an image (for the develop metadata panel).
    std::vector<std::string> keywordFullPathsForImage(int imageId) const;
    // Unchecks the spot-tool toolbar button without emitting (used when a mask
    // tool or sampler disarms the spot tool inside the viewport).
    void disarmSpotToolButton();

    // --- Duplicate detection helpers -----------------------------------------
    /**
     * @brief Result of a background duplicate scan.
     *
     * Plain data so it can travel through QFutureWatcher; `error` is non-empty
     * when the scan could not run at all (no catalog / too few thumbnails).
     */
    struct DuplicateScanResult {
        std::vector<lrc::DuplicateGroup> groups;
        size_t photosScanned = 0;
        size_t embeddedCount = 0;
        QString error;
    };
    /**
     * @brief Scans the catalog for duplicates. Runs on a worker thread.
     *
     * Loads each photo's catalog thumbnail into an ImageBuffer (never touches
     * widgets), seeds DuplicateDetector with the cached phash/embedding rows, and
     * writes the newly computed features back through storePhotoFeatures() so the
     * next scan is cheaper.
     */
    DuplicateScanResult scanForDuplicates();
    // Non-null while a scan is in flight (also gates re-entrant scans).
    QFutureWatcher<DuplicateScanResult>* duplicateWatcher_ = nullptr;
    // Linear F32 -> linear U16 (panorama DNG writer needs U16).
    lrc::ImageBufferPtr f32ToU16(const lrc::ImageBufferPtr& f32) const;

    // --- Undo/redo + global shortcuts -----------------------------------------
    // Application-wide key filter that implements the bare-key shortcuts
    // (Z/Tab/F/G/D/E/L/Home/End/[ ]/K/M/O/W/+/-). Modifier combos
    // (Ctrl+Z etc.) are handled by QShortcut instead.
    bool eventFilter(QObject* watched, QEvent* event) override;
    // New user edit: updates the panel + preview, pushes one undo entry.
    void commitAdjustments(const lrc::Adjustments& adjustments);
    // Undo/redo replay: updates the panel + preview but does NOT push.
    void applyAdjustmentState(const lrc::Adjustments& adjustments);
    // Filmstrip navigation helpers (Ctrl+Left/Right, Home/End).
    void navigateImages(int delta);
    void showImageAtIndex(int index);
    // Collapses / expands the left (navigator) + right (develop) panes.
    void setSidePanelsVisible(bool visible);
    void refreshDcpProfiles(const QString& cameraModel);
    void refreshLensfunProfile(const QString& cameraMake,
                               const QString& cameraModel,
                               const QString& lensMaker,
                               const QString& lensModel,
                               float focalLengthMm, float aperture,
                               float cropFactor);

    // Constructs aiEngine_ + modelManager_, loads every ready model from
    // <applicationDirPath>/models/manifest.json into the engine, and publishes
    // the engine via Pipeline::setGlobalAiEngine. Safe to call when LRC_ENABLE_AI
    // is off (the body is #ifdef-guarded to a no-op); safe to call when the
    // bundled manifest is missing (logs + returns).
    void initializeAiEngine();

    // --- engine / catalog --------------------------------------------------
    lrc::RawDecoder decoder_;
    std::shared_ptr<lrc::ColorManager> colorManager_;
    std::unique_ptr<lrc::Pipeline> pipeline_;
    // AI inference engine + model manager. The engine is shared process-wide via
    // Pipeline::setGlobalAiEngine so the static applySpatialAdjustments /
    // applyLocalAdjustments entry points can reach it. Owned here so the engine
    // outlives every preview/export render.
    std::unique_ptr<lrc::AiInferenceEngine> aiEngine_;
    std::unique_ptr<lrc::ModelManager> modelManager_;
    std::unique_ptr<lrc::CatalogDatabase> catalog_;
    std::unique_ptr<lrc::ThumbnailCache> thumbnailCache_;
    std::unique_ptr<lrc::FolderMonitor> folderMonitor_;
    // Named develop-preset store (%APPDATA%/Silver Halide/presets). Created
    // with the window; PresetDialog borrows it without taking ownership.
    std::unique_ptr<lrc::PresetLibrary> presetLibrary_;

    // --- workers --------------------------------------------------------
    ImportWorker* importWorker_ = nullptr;
    QThread* importThread_ = nullptr;
    PreviewWorker* previewWorker_ = nullptr;
    QThread* previewThread_ = nullptr;
    qint64 previewGeneration_ = 0;
    // Catalog backup/optimize run on a std::thread so the UI thread never blocks
    // on sqlite3_backup_step / VACUUM.
    //
    // The thread is never detached: the completion slot joins it (the work is
    // already done by then, so the join cannot block for more than the few
    // instructions left in the worker), and joinDbOpThread() reclaims the handle
    // in the destructor, in closeEvent and before a new operation starts.
    // dbOpRunning_ guards against re-entrant operations.
    std::thread dbOpThread_;
    bool dbOpRunning_ = false;

    // --- widgets --------------------------------------------------------
    NavigatorWidget* navigator_ = nullptr;
    ImageViewport* viewport_ = nullptr;
    DevelopPanel* developPanel_ = nullptr;
    FilmstripWidget* filmstrip_ = nullptr;
    QScrollArea* developScroll_ = nullptr;  // scroll area wrapping developPanel_
    // Pane proportions are user state: both splitters are restored from
    // QSettings on startup and written back on close.
    QSplitter* mainSplitter_ = nullptr;    // navigator | center | develop panel
    QSplitter* centerSplitter_ = nullptr;  // viewport | filmstrip
    QDialog* maskWindow_ = nullptr;
    // Modeless soft-proofing dialog (视图 → 软打样…). Parented to the window so
    // it is destroyed with it. Lives in namespace lrc, hence the qualification.
    lrc::SoftProofingDialog* softProofDialog_ = nullptr;
    // Last parameters pushed by the dialog + whether proofing is on. Kept on the
    // UI thread so a freshly rendered preview can be re-proofed (see
    // onPreviewReady).
    lrc::SoftProofParams softProofParams_;
    bool softProofEnabled_ = false;
    // Destination PNG for the in-flight gamut report request (empty = none).
    QString pendingGamutReportPath_;
    QTimer* folderTimer_ = nullptr;
    // One-shot throttle (150ms) that guarantees the last coarse frame's
    // histogram/status update lands after the throttle window ends.
    QTimer* histogramThrottle_ = nullptr;
    // One-shot debounce (500ms) that coalesces adjustment DB writes.
    QTimer* saveDebounceTimer_ = nullptr;
    // While a background worker is still stopping, the close is deferred and
    // this repeating timer re-closes once every worker has finished.
    QTimer* workerStopPollTimer_ = nullptr;
    // Toolbar groups: the viewport/panel are the source, the toolbar mirrors.
    QButtonGroup* viewButtonGroup_ = nullptr;  // Fit / FitWidth / 1:1
    QButtonGroup* maskButtonGroup_ = nullptr;  // linear / radial / brush
    QAction* spotToolAction_ = nullptr;        // checkable spot-removal tool
    QButtonGroup* compareButtonGroup_ = nullptr;  // Off / L-R / T-B / single
    QLineEdit* searchEdit_ = nullptr;          // top-toolbar search box
    QTimer* searchDebounceTimer_ = nullptr;    // search debounce (300 ms)
    QTimer* spotCommitTimer_ = nullptr;        // spot-gesture commit debounce
    QString currentDcpProfilePath_;

    // --- state ----------------------------------------------------------
    QVector<FilmstripWidget::Entry> entries_;
    QHash<int, lrc::ImageRecord> records_;
    QString libraryRoot_;  // managed-library root (Copy/Move imports land here)
    QString catalogPath_;  // absolute path of the currently open catalog
    QString catalogName_;  // file name of the currently open catalog (e.g. Default.lrcat)
    int currentImageId_ = -1;
    int currentMaskIndex_ = -1;  // mask being edited by the viewport tool
    QString currentFilePath_;
    lrc::Adjustments currentAdjustments_;
    lrc::UndoStack<lrc::Adjustments> undoStack_;  // 50-step undo history
    bool maskStrokeActive_ = false;  // true while a mask stroke is being drawn
    bool gestureActive_ = false;     // a slider-drag gesture is in progress
    bool fullResActive_ = false;  // true while the 1:1 full-res view is live
    // Image id of the most recent final-preview frame applied to the viewport.
    // onPreviewReady resets the view only for the FIRST frame of a different
    // image; same-image re-renders (slider edits, mask strokes) swap pixels
    // without touching zoom/pan, which used to yank the view mid-gesture.
    qint64 lastPreviewImageId_ = -1;
    QString filterFolder_;
    int filterRating_ = 0;       // 0 = any
    int filterFlag_ = 0;         // 0 = any, 1 = picked, -1 = rejected, 2 = none
    int filterLabelColor_ = 0;   // 0 = any, 1..7 = colour label
    int filterCollectionId_ = -1;  // -1 = all collections
    // Additional navigator-driven filter dimensions (mutually exclusive with the
    // folder / collection / search filters; clearing happens in
    // clearImageFilters()).
    QString filterSearchText_;     // non-empty -> restrict to searchImages()
    int64_t filterKeywordId_ = -1;  // keyword filter (-1 = none)
    int64_t filterSmartCollectionId_ = -1;  // smart collection (-1 = none)

    // --- Integration state ----------------------------------------------------
    bool spotStrokeActive_ = false;  // true while a spot gesture is in flight
    // Which edit variant the current photo view shows (0 = master).
    int currentVariantId_ = 0;
    // Variant the debounced adjustment save belongs to (mirror of
    // pendingSaveImageId_).
    int pendingSaveVariantId_ = 0;
    // Export-variant selection (defaults to the currently-viewed variant).
    qint64 currentExportVariantId_ = 0;
    // Catalog id of the reference photo (-1 = reference view off).
    int64_t referenceImageId_ = -1;
    // Navigator virtual-copies group rebuild guard (rebuild only when the
    // library image count changes or after an explicit invalidation).
    bool virtualCopiesBuilt_ = false;
    int lastVcImageCount_ = -1;
    // Source image ids of the panorama currently being merged.
    std::vector<int64_t> panoramaSourceIds_;

    // --- Histogram/status-bar throttle state -----------------------------------
    qint64 lastHistogramMs_ = 0;      // monotonic ms of the last full histogram update
    bool histogramPending_ = false;   // a coarse histogram update is queued
    QElapsedTimer histogramClock_;    // monotonic clock for the throttle
    QImage pendingHistogramImage_;    // most recent coarse frame awaiting flush

    // --- 1:1 entry instrumentation ([PERF1A]) ----------------------------------
    // Monotonic clock started when a 1:1 request leaves the UI thread (t0). The
    // frame that answers that request stops it (t7). Diagnostics only: it never
    // gates, reorders or delays rendering.
    //
    // The pending sample is tagged with the generation and image of the request
    // that opened it. Any newer request (requestFullRes or requestAdjustFullRes,
    // both of which bump previewGeneration_) invalidates the tag, and the sample
    // is consumed by the first frame that matches, so a later frame can never be
    // charged to an earlier request and report an ever-growing total.
    QElapsedTimer perfFullResClock_;
    qint64 perfFullResGeneration_ = -1;  // generation of the measured 1:1 entry
    qint64 perfFullResImageId_ = -1;     // image that entry was requested for

    // --- Debounced adjustment persistence --------------------------------------
    std::optional<lrc::Adjustments> pendingSave_;  // latest value awaiting flush
    int pendingSaveImageId_ = -1;     // image the pending save belongs to

    // --- Filmstrip lazy-thumbnail background decoding ---------------------------
    QThread* thumbLoaderThread_ = nullptr;  // thread that decodes thumbnails
    QObject* thumbLoaderHub_ = nullptr;     // lives on thumbLoaderThread_, runs decode
    QVector<int> pendingThumbnails_;        // imageIds awaiting decode (bounded FIFO)
    QSet<int> queuedThumbnails_;            // pending + in-flight dedupe set
    bool thumbDecodeBusy_ = false;          // one decode in flight on the thumb thread
};
