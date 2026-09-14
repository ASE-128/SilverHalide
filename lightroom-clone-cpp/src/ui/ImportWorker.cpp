#include "ui/ImportWorker.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QFileInfo>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QThreadPool>
#include <QtConcurrent>

#include "app/Constants.h"
#include "catalog/CatalogDatabase.h"
#include "catalog/FolderMonitor.h"
#include "catalog/LibraryManager.h"
#include "catalog/StandardPreview.h"
#include "catalog/ThumbnailCache.h"
#include "engine/ColorManager.h"
#include "engine/DngConverter.h"
#include "engine/Pipeline.h"
#include "engine/RawDecoder.h"
#include "utils/RenameTemplate.h"

namespace {

// S3-4: import batch granularity. Each commit absorbs up to this many image
// rows + preview records so a crash mid-batch only loses the last partial
// batch (≤ kImportBatchSize rows) instead of the whole import.
constexpr int kImportBatchSize = 100;

// S3-4: stage-2 thread pool ceiling. Clamped to [2, 6] so libraw's own
// internal worker threads (typically 2-3) plus the stage-2 workers stay
// close to the hardware concurrency budget instead of oversubscribing.
constexpr int kStage2MinThreads = 2;
constexpr int kStage2MaxThreads = 6;

std::filesystem::path toStdPath(const QString& path) {
    return std::filesystem::path(path.toStdWString());
}

QString fromStdPath(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

// Case-insensitive ".dng" check (Windows file systems are case-insensitive).
bool isDngFile(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".dng";
}

// Local calendar date of `p`'s modification time as "YYYY-MM-DD", falling back
// to *now* when the filesystem query fails. Mirrors LibraryManager::sourceDate
// so the {date} token agrees with the by-date import folder layout.
std::string sourceDateString(const std::filesystem::path& p) {
    using namespace std::chrono;
    std::error_code ec;
    auto ft = std::filesystem::last_write_time(p, ec);
    std::time_t t;
    if (ec) {
        t = system_clock::to_time_t(system_clock::now());
    } else {
        const auto sct = time_point_cast<system_clock::duration>(
            ft - file_clock::now() + system_clock::now());
        t = system_clock::to_time_t(sct);
    }
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

// Reads the on-disk fingerprint (byte size + unix last-write-time) of `p` for
// the images.file_size/file_mtime columns (goal 4). Returns false when the file
// cannot be stat()ted; callers then pass (0,0) = "unknown / keep current".
bool fileFingerprint(const std::filesystem::path& p, int64_t& size,
                     int64_t& mtime) {
    std::error_code ec;
    const auto fs = std::filesystem::file_size(p, ec);
    if (ec)
        return false;
    size = static_cast<int64_t>(fs);
    auto ft = std::filesystem::last_write_time(p, ec);
    if (ec) {
        mtime = 0;
        return true;
    }
    // Convert the filesystem clock's last-write-time to a unix time_t: shift by
    // (now on both clocks) and cast into system_clock::duration. Mirrors the
    // working conversion in sourceDateString above.
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - std::filesystem::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    mtime = static_cast<int64_t>(std::chrono::system_clock::to_time_t(sys));
    return true;
}

} // namespace

ImportWorker::ImportWorker(lrc::CatalogDatabase& catalog,
                           lrc::ThumbnailCache& cache,
                           lrc::RawDecoder& decoder, lrc::Pipeline& pipeline,
                           std::shared_ptr<const lrc::ColorManager> colorManager,
                           QObject* parent)
    : QObject(parent),
      catalog_(catalog),
      cache_(cache),
      decoder_(decoder),
      pipeline_(pipeline),
      cm_(std::move(colorManager)) {}

void ImportWorker::importPaths(const QStringList& paths, bool convertToDng,
                               lrc::ImportMode mode, const QString& destRoot,
                               const QString& renameTemplate) {
    // A Copy/Move without a destination folder is meaningless; degrade to Add
    // so the catalog never loses the file.
    if (mode != lrc::ImportMode::Add && destRoot.isEmpty())
        mode = lrc::ImportMode::Add;

    if (abort_.load())
        return;  // 已请求取消:直接退出 (abort requested: bail out immediately).
    const std::filesystem::path root = toStdPath(destRoot);
    const int total = paths.size();
    int done = 0;
    int added = 0;

    // S3-4 stage-1 conversion failures (shared across QtConcurrent tasks).
    std::atomic<int> conversionFailures{0};
    QMutex firstErrorMutex;
    std::string firstError;

    // S3: imports share the single catalog connection (goal 5). A batch
    // transaction is opened just before the DB-serial stage and committed every
    // kImportBatchSize rows; UI-thread writes queue on the single connection via
    // the busy_timeout, so a cancelled import's rollback can never undo an edit
    // the user saved while the import ran.

    // ===================================================================
    // Stage 1 (IO + DNG convert): produces the on-disk catalog path each file
    // will be registered under. DNG conversion + file copy/move is IO-bound
    // with light CPU (the heavy libraw decode runs in stage 2); running the
    // per-file work on QtConcurrent lets multiple files convert in parallel.
    // Stage 1 never touches the catalog so it runs *outside* the import
    // transaction (the transaction is opened above only to keep the begin/commit
    // pair balanced if stage 1 aborts early).
    // ===================================================================
    std::vector<PerFileStage1> stage1;
    stage1.reserve(static_cast<size_t>(total));
    int seq = 1;
    for (const QString& path : paths) {
        PerFileStage1 f;
        f.src = path;
        f.seqIndex = seq++;  // stable 1-based number in selection order
        f.catalogPath = toStdPath(path);
        stage1.push_back(std::move(f));
    }

    QtConcurrent::blockingMap(stage1, [&](PerFileStage1& f) {
        if (abort_.load() || !f.ok) {
            // f.ok stays false on abort; the consumer treats ok=false (with
            // an empty warn) as "skipped, do not report" so aborted files do
            // not emit a warning storm.
            return;
        }
        const std::filesystem::path src = toStdPath(f.src);
        std::filesystem::path catalogPath = src;
        bool converted = false;

        // "Import as DNG": convert every non-DNG RAW to a .dng and catalog the
        // DNG. In Add mode the DNG is written beside the source; in Copy/Move
        // mode it is written into the managed library. A failed conversion
        // falls back to the original RAW so the user's files are never lost.
        if (convertToDng && lrc::FolderMonitor::isRawFile(src) &&
            !isDngFile(src)) {
            std::filesystem::path outDng;
            if (mode == lrc::ImportMode::Add) {
                outDng = lrc::DngConverter::suggestDngPath(src);
            } else {
                outDng = lrc::LibraryManager::suggestDestPath(src, root);
                outDng.replace_extension(".dng");
            }
            std::string convertError;
            if (lrc::DngConverter::convert(src, outDng, &convertError)) {
                catalogPath = outDng;
                converted = true;
                if (mode == lrc::ImportMode::Move && outDng != src) {
                    // The DNG already lives in the library; drop the source RAW
                    // to complete the move. A leftover source is not fatal but
                    // must be surfaced so the user knows the move was in effect
                    // a copy (M4).
                    std::error_code ec;
                    if (!std::filesystem::remove(src, ec) && ec)
                        f.warn =
                            tr("无法删除源文件 %1（已生成 DNG，源可能残留）：%2")
                                .arg(f.src)
                                .arg(QString::fromStdString(ec.message()));
                }
            } else {
                conversionFailures.fetch_add(1, std::memory_order_relaxed);
                {
                    QMutexLocker lock(&firstErrorMutex);
                    if (firstError.empty())
                        firstError = convertError;
                }
                // catalogPath stays src: fall through to library handling.
            }
        }

        // Copy/Move (or the original RAW after a failed conversion) goes
        // through the library import policy. Add returns the source unchanged.
        if (!converted) {
            std::string error;
            const auto result =
                lrc::LibraryManager::importFile(src, root, mode, &error);
            if (!result) {
                f.warn = tr("跳过 %1：%2")
                             .arg(f.src)
                             .arg(QString::fromStdString(error));
                // ok stays false: stage 3 will skip this file.
                return;
            }
            // M4: importFile returns the destination even when a Move's copy
            // succeeded but its source-deletion step failed (error is then set
            // and the source may still be on disk). Surface it so the user is
            // not led to believe the move actually moved the file.
            if (!error.empty())
                f.warn = tr("%1：%2")
                             .arg(f.src)
                             .arg(QString::fromStdString(error));
            catalogPath = *result;
        }

        f.catalogPath = std::move(catalogPath);
        f.convertedToDng = converted;

        // P1-3: rename on import. Applies to the finalized cataloged path (the
        // file in place, the copied/moved file, or the converted DNG). {date}
        // uses the source file's modification date, {index} the per-file
        // selection-order number, and collisions get a numeric suffix.
        if (!renameTemplate.isEmpty()) {
            const std::filesystem::path cur = f.catalogPath;
            const lrc::RenameTemplateArgs ra{
                renameTemplate.toStdString(),    // tmpl
                cur.stem().string(),             // base
                f.seqIndex,                      // index
                2,                               // indexWidth
                sourceDateString(toStdPath(f.src))};  // date
            const std::filesystem::path newPath =
                lrc::uniqueRenamePath(cur.parent_path(), ra,
                                      cur.extension().string());
            if (newPath != cur) {
                // Serialize uniqueness check + rename so two files in this batch
                // that render to the same name cannot both claim it: the first
                // rename moves its file, making the name visible on disk for the
                // next one's uniqueRenamePath check.
                std::error_code erc;
                {
                    QMutexLocker lock(&renameMutex_);
                    if (!std::filesystem::exists(newPath)) {
                        std::filesystem::rename(cur, newPath, erc);
                        if (!erc)
                            f.catalogPath = newPath;
                    }
                }
                if (erc)
                    f.warn = tr("无法重命名 %1：%2")
                                 .arg(fromStdPath(cur))
                                 .arg(QString::fromStdString(erc.message()));
            }
        }

        f.ok = true;
    });

    if (abort_.load()) {
        // 取消时回滚未提交的插入,不 emit finishedImport (roll back the
        // pending inserts and return without finishedImport).
        if (!catalog_.rollbackTransaction())
            emit importWarning(tr("取消导入时回滚失败：%1")
                                   .arg(QString::fromStdString(
                                       catalog_.lastError())));
        return;
    }

    // ===================================================================
    // Stage 2 (CPU-heavy decode + render): for every successfully prepared
    // file, build the thumbnail JPEG bytes and the standard preview blob in
    // parallel. The RawDecoder and Pipeline are *not* thread-safe across
    // concurrent decodes, so each task gets its own instances built from the
    // shared ColorManager — this keeps colour transforms consistent while
    // avoiding per-pixel cross-thread sharing of stateful objects.
    // ===================================================================
    std::vector<PerFileFinal> finals;
    finals.reserve(stage1.size());
    for (auto& s : stage1) {
        PerFileFinal f;
        f.src = s.src;
        f.catalogPath = std::move(s.catalogPath);
        f.ok = s.ok;
        f.warn = std::move(s.warn);
        f.convertedToDng = s.convertedToDng;
        finals.push_back(std::move(f));
    }
    stage1.clear();
    stage1.shrink_to_fit();

    QThreadPool stage2Pool;
    const int ideal = QThread::idealThreadCount();
    stage2Pool.setMaxThreadCount(
        std::clamp(ideal / 2, kStage2MinThreads, kStage2MaxThreads));

    QtConcurrent::blockingMap(&stage2Pool, finals, [&](PerFileFinal& f) {
        if (abort_.load() || !f.ok)
            return;
        // Per-task colour manager + decoder + pipeline. cm_ is shared (const,
        // thread-safe) so every task sees the same lcms transforms.
        lrc::RawDecoder decoder;
        decoder.setColorManager(cm_);
        lrc::Pipeline pipeline(cm_);

        // Thumbnail JPEG (small, filmstrip): reduced-size decode + render.
        if (auto thumb = cache_.renderThumbJpeg(
                f.catalogPath, lrc::app::kThumbnailDim, decoder, pipeline)) {
            f.thumb = std::move(*thumb);
        }
        // Standard preview blob (2048px linear ProPhoto F32): self-contained,
        // builds its own decoder/colour manager internally.
        if (auto stdPrev = cache_.renderStandardPreviewBlob(f.catalogPath)) {
            f.standard = std::move(*stdPrev);
        }
    });

    if (abort_.load()) {
        if (!catalog_.rollbackTransaction())
            emit importWarning(tr("取消导入时回滚失败：%1")
                                   .arg(QString::fromStdString(
                                       catalog_.lastError())));
        return;
    }

    // ===================================================================
    // Stage 3 (DB-serial, batched commit): insert one row per file plus its
    // preview records, committing every kImportBatchSize rows so a crash or
    // abort loses at most the last partial batch instead of the whole import.
    // ===================================================================
    if (!catalog_.beginTransaction()) {
        emit finishedImport(0);
        emit importWarning(tr("导入事务启动失败：%1")
                               .arg(QString::fromStdString(
                                   catalog_.lastError())));
        return;
    }
    int committedBatchRows = 0;
    int rowsInBatch = 0;

    auto flushBatch = [&]() -> bool {
        if (rowsInBatch == 0)
            return true;
        if (!catalog_.commitTransaction()) {
            // S2: a failed COMMIT leaves the transaction open; CatalogDatabase
            // rolled it back internally. Report the failure and abort: only the
            // already-committed batches landed.
            emit importWarning(tr("导入批次提交失败：%1")
                                   .arg(QString::fromStdString(
                                       catalog_.lastError())));
            return false;
        }
        if (!catalog_.beginTransaction()) {
            emit importWarning(tr("导入事务重启失败：%1")
                                   .arg(QString::fromStdString(
                                       catalog_.lastError())));
            return false;
        }
        committedBatchRows += rowsInBatch;
        rowsInBatch = 0;
        return true;
    };

    for (auto& f : finals) {
        if (abort_.load())
            break;

        if (!f.ok) {
            // Stage 1 reported a skip/failure. Surface the warning (if any)
            // and advance the progress counter so the UI does not stall.
            if (!f.warn.isEmpty())
                emit importWarning(f.warn);
            ++done;
            emit progress(done, total);
            continue;
        }

        const std::string dbPath = fromStdPath(f.catalogPath).toStdString();
        // Record the on-disk fingerprint (byte size + mtime, goal 4) so later
        // external modifications can be detected and re-imports stay idempotent.
        int64_t fsize = 0, fmtime = 0;
        (void)fileFingerprint(f.catalogPath, fsize, fmtime);
        const int64_t id = catalog_.addImage(
            dbPath, std::string{}, mode != lrc::ImportMode::Add, fsize, fmtime);
        if (id <= 0) {
            emit importWarning(tr("写入目录失败：%1")
                                   .arg(QString::fromStdString(
                                       catalog_.lastError())));
            ++done;
            emit progress(done, total);
            continue;
        }

        // Thumbnail preview + standard preview land in the same import
        // transaction so they commit atomically with the image row. The full-
        // res (1:1) preview stays lazy (M5-3): PreviewWorker builds it on the
        // first 1:1 view, so import is not bottlenecked on a full-res render
        // per photo.
        if (f.thumb) {
            catalog_.storePreview(id, lrc::PreviewKind::Thumbnail,
                                  f.thumb->bytes, f.thumb->width,
                                  f.thumb->height);
            const int imageId = static_cast<int>(id);
            // M2-3: decode the just-encoded JPEG in the worker thread and
            // ship a ready QImage to the UI (queued connection, no
            // qRegisterMetaType needed -- QImage is a built-in metatype).
            QImage thumbImage;
            if (thumbImage.loadFromData(QByteArray(
                    reinterpret_cast<const char*>(f.thumb->bytes.data()),
                    static_cast<int>(f.thumb->bytes.size())))) {
                emit thumbReadyImage(imageId, thumbImage);
            } else {
                // Rare: the JPEG we just encoded failed to re-decode; keep
                // the legacy signal so MainWindow's onThumbReady fallback can
                // retry from the embedded blob / disk path.
                emit thumbReady(imageId, QString());
            }
        }
        if (f.standard) {
            // The standard preview blob is stored verbatim (header v2 +
            // payload). Storing via storePreview(Standard) skips a wasteful
            // ImageBuffer round-trip — the v2 blob already carries
            // width/height/nativeW/nativeH, and decodeStandardPreview on read
            // reads them back directly.
            catalog_.storePreview(
                id, lrc::PreviewKind::Standard, f.standard->blob,
                f.standard->previewWidth, f.standard->previewHeight);
        }

        ++rowsInBatch;
        ++added;
        ++done;
        emit progress(done, total);

        // Commit every kImportBatchSize rows so a crash never loses more
        // than the last partial batch.
        if (rowsInBatch >= kImportBatchSize) {
            if (!flushBatch()) {
                // Failed to commit/restart: stop now, but keep what landed.
                // added already excludes the failed batch's rows (we did not
                // bump committedBatchRows); emit finishedImport with the real
                // committed count.
                added = committedBatchRows;
                emit finishedImport(added);
                if (conversionFailures.load() > 0)
                    emit importWarning(
                        tr("%1 个文件无法转换为 DNG，已按原 RAW 导入。%2")
                            .arg(conversionFailures.load())
                            .arg(QString::fromStdString(firstError)));
                return;
            }
            // After a successful batch commit, the uncommitted count is 0;
            // `added` so far equals committedBatchRows.
            added = committedBatchRows;
        }
    }

    // Final flush: commit any tail rows that did not fill a full batch.
    if (rowsInBatch > 0) {
        if (!flushBatch()) {
            added = committedBatchRows;
            emit finishedImport(added);
            if (conversionFailures.load() > 0)
                emit importWarning(
                    tr("%1 个文件无法转换为 DNG，已按原 RAW 导入。%2")
                        .arg(conversionFailures.load())
                        .arg(QString::fromStdString(firstError)));
            return;
        }
        added = committedBatchRows;
    }

    // The transaction is now either committed (final flush) or rolled back
    // (abort path handled above). `added` reflects only the rows that actually
    // landed on disk, so the UI count never lies about partial success.
    emit finishedImport(added);
    if (conversionFailures.load() > 0)
        emit importWarning(tr("%1 个文件无法转换为 DNG，已按原 RAW 导入。%2")
                               .arg(conversionFailures.load())
                               .arg(QString::fromStdString(firstError)));
}
