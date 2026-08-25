#pragma once

// Background import worker: adds raw files to the catalog and builds previews
// (thumbnail + full-res JPEG) that are embedded in the catalog file at import
// time, without blocking the UI. Runs in its own QThread. All engine objects
// it touches (database, thumbnail cache, decoder, pipeline) are internally
// thread-safe. The whole batch is wrapped in one SQLite transaction so a large
// import commits once instead of once per file.

#include <atomic>

#include <QImage>
#include <QMetaType>
#include <QObject>
#include <QStringList>

#include "catalog/LibraryManager.h"

namespace lrc {
class CatalogDatabase;
class Pipeline;
class RawDecoder;
class ThumbnailCache;
} // namespace lrc

class ImportWorker : public QObject {
    Q_OBJECT
public:
    ImportWorker(lrc::CatalogDatabase& catalog, lrc::ThumbnailCache& cache,
                 lrc::RawDecoder& decoder, lrc::Pipeline& pipeline,
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
    void importPaths(const QStringList& paths, bool convertToDng,
                     lrc::ImportMode mode = lrc::ImportMode::Add,
                     const QString& destRoot = QString());

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
    std::atomic<bool> abort_{false};
};

// Registered in main() so the enum can cross a queued connection (ImportWorker
// thread -> UI thread) as part of the importPaths invocation arguments.
Q_DECLARE_METATYPE(lrc::ImportMode)
