#include "ui/ImportWorker.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>
#include <utility>

#include <QByteArray>
#include <QFileInfo>
#include <QImage>

#include "app/Constants.h"
#include "catalog/CatalogDatabase.h"
#include "catalog/FolderMonitor.h"
#include "catalog/LibraryManager.h"
#include "catalog/StandardPreview.h"
#include "catalog/ThumbnailCache.h"
#include "engine/DngConverter.h"
#include "engine/Pipeline.h"
#include "engine/RawDecoder.h"

namespace {

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

} // namespace

ImportWorker::ImportWorker(lrc::CatalogDatabase& catalog,
                           lrc::ThumbnailCache& cache,
                           lrc::RawDecoder& decoder, lrc::Pipeline& pipeline,
                           QObject* parent)
    : QObject(parent),
      catalog_(catalog),
      cache_(cache),
      decoder_(decoder),
      pipeline_(pipeline) {}

void ImportWorker::importPaths(const QStringList& paths, bool convertToDng,
                               lrc::ImportMode mode, const QString& destRoot) {
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
    int conversionFailures = 0;
    std::string firstError;
    // Successfully inserted (imageId, cataloged file path). Standard previews
    // are built from these after the import transaction commits (see below).
    std::vector<std::pair<int64_t, std::string>> addedImages;

    // S3: the import runs on its own catalog connection so the batch
    // transaction is fully isolated from UI-thread writes (ratings /
    // adjustments) on the primary connection. A cancelled import's rollback can
    // therefore never undo an edit the user saved while the import ran.
    if (!catalog_.openImportConnection()) {
        // finishedImport first so the UI clears its "importing" state, then the
        // warning (longer status display) is the final message the user sees.
        emit finishedImport(0);
        emit importWarning(tr("无法为导入打开目录连接：%1")
                               .arg(QString::fromStdString(
                                   catalog_.importLastError())));
        return;
    }
    // RAII: close the import connection on every exit path so the handle is
    // released and no file handle stays open after the import.
    struct ImportConnectionGuard {
        lrc::CatalogDatabase& db;
        ~ImportConnectionGuard() { db.closeImportConnection(); }
    } importGuard{catalog_};

    // 整批一个 SQLite 事务,避免每个文件单独提交 (one transaction for the whole
    // batch so sqlite does not commit once per file; large imports run much faster).
    if (!catalog_.beginImportTransaction()) {
        emit finishedImport(0);
        emit importWarning(tr("导入事务启动失败：%1")
                               .arg(QString::fromStdString(
                                   catalog_.importLastError())));
        return;
    }

    for (const QString& path : paths) {
        if (abort_.load()) {
            // 取消时回滚未提交的插入,不 emit finishedImport (roll back the
            // pending inserts and return without finishedImport).
            if (!catalog_.rollbackImportTransaction())
                emit importWarning(tr("取消导入时回滚失败：%1")
                                       .arg(QString::fromStdString(
                                           catalog_.importLastError())));
            return;
        }
        const std::filesystem::path src = toStdPath(path);
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
                        emit importWarning(
                            tr("无法删除源文件 %1（已生成 DNG，源可能残留）：%2")
                                .arg(path)
                                .arg(QString::fromStdString(ec.message())));
                }
            } else {
                ++conversionFailures;
                if (firstError.empty())
                    firstError = convertError;
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
                emit importWarning(tr("跳过 %1：%2")
                                       .arg(path)
                                       .arg(QString::fromStdString(error)));
                ++done;
                emit progress(done, total);
                continue;
            }
            // M4: importFile returns the destination even when a Move's copy
            // succeeded but its source-deletion step failed (error is then set
            // and the source may still be on disk). Surface it so the user is
            // not led to believe the move actually moved the file.
            if (!error.empty())
                emit importWarning(tr("%1：%2")
                                       .arg(path)
                                       .arg(QString::fromStdString(error)));
            catalogPath = *result;
        }

        const std::string dbPath = fromStdPath(catalogPath).toStdString();
        const int64_t id = catalog_.addImageImport(
            dbPath, std::string{}, mode != lrc::ImportMode::Add);
        if (id > 0) {
            // 导入时构建预览并内嵌进目录,不写外部磁盘缓存 (build previews at import
            // time and embed them in the catalog; no external disk cache).
            if (auto thumb = cache_.renderThumbJpeg(
                    catalogPath, lrc::app::kThumbnailDim, decoder_, pipeline_)) {
                catalog_.storePreviewImport(id, lrc::PreviewKind::Thumbnail,
                                            thumb->bytes, thumb->width,
                                            thumb->height);
                const int imageId = static_cast<int>(id);
                // M2-3: decode the just-encoded JPEG in the worker thread and
                // ship a ready QImage to the UI (queued connection, no
                // qRegisterMetaType needed -- QImage is a built-in metatype).
                // The main thread no longer does a synchronous disk decode.
                QImage thumbImage;
                if (thumbImage.loadFromData(
                        QByteArray(reinterpret_cast<const char*>(thumb->bytes.data()),
                                   static_cast<int>(thumb->bytes.size())))) {
                    emit thumbReadyImage(imageId, thumbImage);
                } else {
                    // Rare: the JPEG we just encoded failed to re-decode; keep
                    // the legacy signal so MainWindow's onThumbReady fallback
                    // can retry from the embedded blob / disk path.
                    emit thumbReady(imageId, QString());
                }
            }
            // M5-3: 1:1 fullres is no longer pre-generated at import time (it
            // was the import bottleneck). PreviewWorker::loadFullRes builds it
            // lazily on the first 1:1 view (decode + render + write-back to the
            // catalog previews table).
            // Remember the row so the standard preview can be built after the
            // batch transaction commits (it must not extend the transaction).
            addedImages.emplace_back(id, dbPath);
            ++added;
        } else {
            // A real DB error (disk full, locked file): report it instead of
            // silently skipping the record.
            emit importWarning(tr("写入目录失败：%1")
                                   .arg(QString::fromStdString(
                                       catalog_.importLastError())));
        }
        ++done;
        emit progress(done, total);
    }

    if (!catalog_.commitImportTransaction()) {
        // S2: a failed COMMIT (disk full / file locked) leaves the transaction
        // open; CatalogDatabase rolled it back internally so the import
        // connection is clean again. Report the failure and do NOT pretend the
        // import succeeded -- zero records actually landed.
        emit finishedImport(0);
        emit importWarning(tr("导入提交失败，未写入任何记录：%1")
                               .arg(QString::fromStdString(
                                   catalog_.importLastError())));
        return;
    }

    emit finishedImport(added);
    if (conversionFailures > 0)
        emit importWarning(tr("%1 个文件无法转换为 DNG，已按原 RAW 导入。%2")
                               .arg(conversionFailures)
                               .arg(QString::fromStdString(firstError)));

    // Standard previews (2048px linear ProPhoto F32) are generated after the
    // import transaction commits, so they never extend the batch transaction
    // or block the per-file import loop. Best-effort: a decode/render failure
    // only skips that photo -- the catalog already holds the thumbnail preview
    // (1:1 fullres is built lazily on first 1:1 view), so the UI never depends
    // on the standard preview existing.
    for (const auto& [imageId, path] : addedImages) {
        if (abort_.load())
            break; // shutdown requested: stop without blocking the exit.
        auto stdPrev = lrc::buildStandardPreview(path);
        if (stdPrev && !stdPrev->empty())
            catalog_.storeStandardPreview(imageId, *stdPrev);
    }
}
