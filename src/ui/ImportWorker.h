#pragma once

// Background import worker: adds raw files to the catalog and builds previews
// (thumbnail + standard preview) that are embedded in the catalog file at
// import time, without blocking the UI. Runs in its own QThread.
//
// S3-4: the import runs as a three-stage pipeline so the IO-bound (DNG
// conversion + file copy/move), CPU-bound (RAW decode + thumbnail/standard
// preview rendering) and DB-bound (catalog inserts) stages no longer block
// each other. Stage 1 runs on QtConcurrent (IO + light CPU); Stage 2 runs on
// a dedicated QThreadPool (heavy decode + render); Stage 3 writes the catalog
// in batches of 100 rows per commit so a crash mid-import only loses the last
// partial batch instead of the whole import.
//
// All engine objects the worker touches (database, thumbnail cache) are
// thread-safe. The decoder and pipeline are *not*, so each Stage-2 task owns
// its own RawDecoder + Pipeline built from a shared ColorManager — that keeps
// the colour transforms consistent while avoiding per-pixel cross-thread
// sharing of stateful objects.

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
                 QObject* parent = nullptr);

    // 请求取消:importPaths 在下一轮循环退出 (request shutdown; the running
    // import returns at the next loop iteration). Thread-safe.
    void requestAbort() { abort_.store(true); }

public slots:
    // `convertToDng`: when true, each RAW file (ARW/SR2/...) is converted to a
    // .dng before being cataloged; DNG inputs pass through.
    //
    // `mode` + `destRoot`: Lightroom-style import policy. Add references files
    // in place (DNG is written beside the source); Copy/Move place files into
    // the managed library rooted at `destRoot` (<root>/<yyyy>/<yyyy-mm-dd>/).
    // When `destRoot` is empty a Copy/Move degrades to Add so a caller without
    // a destination can never lose files.
    //
    // `renameTemplate` (P1-3): when non-empty, each file is renamed on import
    // to the rendered template ({date}/{index}/{base}) with a numeric suffix
    // on collision, before it is cataloged. Empty disables renaming.
    void importPaths(const QStringList& paths, bool convertToDng,
                     lrc::ImportMode mode = lrc::ImportMode::Add,
                     const QString& destRoot = QString(),
                     const QString& renameTemplate = QString());

signals:
    // M2-3: the thumbnail JPEG was decoded on the worker thread; the UI thread
    // only applies it. Prefer this over thumbReady (which still carries the
    // legacy path fallback for the rare case the worker decode fails).
    void thumbReadyImage(int imageId, const QImage& thumb);
    void thumbReady(int imageId, const QString& thumbnailPath);
    void progress(int done, int total);
    void finishedImport(int addedCount);
    // Non-fatal conversion failures (the RAW is still imported as-is).
    void importWarning(const QString& message);

private:
    lrc::CatalogDatabase& catalog_;
    lrc::ThumbnailCache& cache_;
    lrc::RawDecoder& decoder_;
    lrc::Pipeline& pipeline_;
    std::shared_ptr<const lrc::ColorManager> cm_;
    std::atomic<bool> abort_{false};
    // P1-3: serializes the on-disk uniqueness check + rename across the
    // concurrent Stage-1 tasks so a batch can never claim the same name twice.
    QMutex renameMutex_;

    // S3-4 pipeline stage records. `PerFileStage1` is the output of the
    // IO/DNG stage (Stage 1); `PerFileFinal` carries the render output that
    // Stage 2 produces, and is consumed by the DB-batch stage (Stage 3). The
    // `imageId` is assigned by Stage 3 (only after the row is in the catalog)
    // so the field stays 0 between Stages 1 and 3.
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
