#pragma once

/**
 * @brief Background import worker: adds raw files to the catalog and builds
 *        embedded previews (thumbnail + standard preview) without blocking the
 *        UI. Lives on its own QThread.
 *
 * The import runs as a three-stage pipeline so the IO-bound (DNG conversion +
 * file copy/move), CPU-bound (RAW decode + thumbnail/standard preview render)
 * and DB-bound (catalog inserts) stages no longer block each other. Stage 1
 * runs on QtConcurrent (IO + light CPU); Stage 2 runs on a dedicated
 * QThreadPool (heavy decode + render); Stage 3 writes the catalog in batches of
 * 100 rows per commit so a crash mid-import only loses the last partial batch.
 *
 * All engine objects the worker touches (database, thumbnail cache) are
 * thread-safe. The decoder and pipeline are *not*, so each Stage-2 task owns
 * its own RawDecoder + Pipeline built from a shared ColorManager -- that keeps
 * the colour transforms consistent while avoiding per-pixel cross-thread
 * sharing of stateful objects.
 */

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include <QImage>
#include <QMetaType>
#include <QMutex>
#include <QObject>
#include <QStringList>

#include "catalog/LibraryManager.h"
#include "catalog/ThumbnailCache.h"

namespace lrc {
class AiInferenceEngine;
class CatalogDatabase;
class ColorManager;
class Pipeline;
class RawDecoder;
} // namespace lrc

class ImportWorker : public QObject {
    Q_OBJECT
public:
    ImportWorker(lrc::CatalogDatabase& catalog, lrc::ThumbnailCache& cache,
                 lrc::RawDecoder& decoder, lrc::Pipeline& pipeline,
                 std::shared_ptr<const lrc::ColorManager> colorManager,
                 lrc::AiInferenceEngine* aiEngine = nullptr,
                 QObject* parent = nullptr);

    /**
     * @brief Requests that the current import abort. Thread-safe; the running
     *        import returns at the next loop iteration.
     *
     * The flag is one-shot per import run: importPaths() clears it at entry,
     * so an abort requested during a previous import (or an idle shutdown
     * request from a deferred close) can never silently swallow the next
     * import.
     */
    void requestAbort() { abort_.store(true); }

public slots:
    /**
     * @brief Imports a set of raw files into the catalog, building embedded
     *        previews so the UI does not block.
     *
     * @param paths File paths to import.
     * @param convertToDng When true, each RAW file is converted to a .dng
     *                     before being cataloged; DNG inputs pass through.
     * @param mode + destRoot Lightroom-style import policy. Add references
     *                    files in place (DNG is written beside the source);
     *                    Copy/Move place files into the managed library rooted
     *                    at destRoot (<root>/<yyyy>/<yyyy-mm-dd>/). When
     *                    destRoot is empty a Copy/Move degrades to Add so a
     *                    caller without a destination can never lose files.
     * @param renameTemplate When non-empty, each file is renamed on import to
     *                       the rendered template ({date}/{index}/{base}) with
     *                       a numeric suffix on collision, before it is
     *                       cataloged. Empty disables renaming.
     */
    void importPaths(const QStringList& paths, bool convertToDng,
                     lrc::ImportMode mode = lrc::ImportMode::Add,
                     const QString& destRoot = QString(),
                     const QString& renameTemplate = QString());

signals:
    /**
     * @brief The thumbnail JPEG was decoded on the worker thread; the UI thread
     *        only applies it. Prefer this over thumbReady.
     */
    void thumbReadyImage(int imageId, const QImage& thumb);
    /**
     * @brief Thumbnail path delivered via the legacy path fallback, kept for
     *        the rare case the worker decode fails.
     */
    void thumbReady(int imageId, const QString& thumbnailPath);
    void progress(int done, int total);
    void finishedImport(int addedCount);
    /**
     * @brief Non-fatal conversion failure (the RAW is still imported as-is).
     */
    void importWarning(const QString& message);
    /**
     * @brief A per-image pregrade marker was produced on the worker thread and
     *        persisted to the catalog (B-2). Delivered after `finishedImport`.
     *        `score` is in [0,100]; `hasSubject`/`suggestAuto` are booleans.
     */
    void pregradeReady(int imageId, bool hasSubject, int score, bool suggestAuto);

private:
    lrc::CatalogDatabase& catalog_;
    lrc::ThumbnailCache& cache_;
    lrc::RawDecoder& decoder_;
    lrc::Pipeline& pipeline_;
    std::shared_ptr<const lrc::ColorManager> cm_;
    // Optional shared AI engine for the rmbg2-subject pregrade path. May be
    // null: PregradeAnalyzer then uses its deterministic saliency fallback so a
    // missing/unloadable model never blocks the batch.
    lrc::AiInferenceEngine* aiEngine_ = nullptr;
    std::atomic<bool> abort_{false};

    /**
     * @brief Three-stage import body (IO/DNG -> decode/render -> DB batches).
     *
     * Runs inside the exception firewall of importPaths(); never called
     * directly. See importPaths() for the failure-reporting contract.
     */
    void importPathsImpl(const QStringList& paths, bool convertToDng,
                         lrc::ImportMode mode, const QString& destRoot,
                         const QString& renameTemplate);

    // Serializes the on-disk uniqueness check + rename across the concurrent
    // Stage-1 tasks so a batch can never claim the same name twice.
    QMutex renameMutex_;

    /**
     * @brief Pipeline stage records.
     *
     * PerFileStage1 is the output of the IO/DNG stage (Stage 1);
     * PerFileFinal carries the render output that Stage 2 produces, and is
     * consumed by the DB-batch stage (Stage 3). The imageId is assigned by
     * Stage 3 (only after the row is in the catalog) so the field stays 0
     * between Stages 1 and 3.
     */
    struct PerFileStage1 {
        QString src;                       // original path (for diagnostics)
        std::filesystem::path catalogPath; // resolved on-disk path
        int seqIndex = 1;                  // 1-based selection-order number
        bool ok = false;
        QString warn;
        bool convertedToDng = false;
    };

    struct PerFileFinal {
        QString src;
        std::filesystem::path catalogPath;
        bool ok = false;
        QString warn;
        bool convertedToDng = false;
        int64_t imageId = 0;
        std::optional<lrc::ThumbnailCache::RenderedJpeg> thumb;
        std::optional<lrc::ThumbnailCache::RenderedStandardPreview> standard;
        int conversionFailureIndex = -1; // -1 = no failure
    };
};

// Registered in main() so the enum can cross a queued connection (ImportWorker
// thread -> UI thread) as part of the importPaths invocation arguments.
Q_DECLARE_METATYPE(lrc::ImportMode)
