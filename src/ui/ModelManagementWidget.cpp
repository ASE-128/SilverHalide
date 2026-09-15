#include "ui/ModelManagementWidget.h"

#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <filesystem>

namespace {
// Columns in the model table.
constexpr int kColName = 0;
constexpr int kColStatus = 1;
constexpr int kColSize = 2;
constexpr int kColLicense = 3;
constexpr int kColAction = 4;
}  // namespace

ModelManagementWidget::ModelManagementWidget(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("模型管理"));
    setMinimumSize(640, 380);

    auto* root = new QVBoxLayout(this);
    root->setSpacing(10);

    // Intro / cache-path hint.
    auto* intro = new QLabel(
        tr("管理 AI 降噪 / 蒙版模型。已就绪 = 本地文件存在且 SHA256 校验通过。\n"
           "缓存目录：%1")
            .arg(QString::fromLocal8Bit(
                lrc::ModelManager::cacheRoot().string())),
        this);
    intro->setWordWrap(true);
    intro->setProperty("caption", true);
    root->addWidget(intro);

    // Model table.
    table_ = new QTableWidget(0, 5, this);
    table_->setHorizontalHeaderLabels(
        {tr("模型"), tr("状态"), tr("大小"), tr("许可证"), tr("操作")});
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setColumnWidth(kColName, 220);
    table_->setColumnWidth(kColStatus, 160);
    table_->setColumnWidth(kColSize, 90);
    table_->setColumnWidth(kColLicense, 90);
    table_->horizontalHeader()->setStretchLastSection(true);
    root->addWidget(table_, 1);

    // Bottom progress + status.
    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setVisible(false);
    root->addWidget(progressBar_);

    statusLabel_ = new QLabel(QString(), this);
    statusLabel_->setProperty("caption", true);
    root->addWidget(statusLabel_);

    // Bottom buttons.
    auto* bottomRow = new QHBoxLayout;
    auto* openFolderBtn = new QPushButton(tr("打开缓存目录…"), this);
    openFolderBtn->setProperty("secondary", true);
    bottomRow->addWidget(openFolderBtn);
    bottomRow->addStretch(1);
    auto* closeBtn = new QPushButton(tr("关闭"), this);
    closeBtn->setDefault(true);
    bottomRow->addWidget(closeBtn);
    root->addLayout(bottomRow);

    connect(openFolderBtn, &QPushButton::clicked, this,
            &ModelManagementWidget::onOpenCacheFolder);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    refreshTable();
}

void ModelManagementWidget::refreshTable() {
    // Preserve active download handles across a refresh so a re-render (e.g.
    // after an import) doesn't lose track of an in-flight download.
    table_->setRowCount(0);
    rowErrors_.clear();

    const auto& models = mm_.allModels();
    for (int i = 0; i < static_cast<int>(models.size()); ++i) {
        const auto& e = models[i];
        // Hide the internal-only test stub (test_add_2x2) from end users.
        if (e.internalOnly) continue;

        const int row = table_->rowCount();
        table_->insertRow(row);

        // Name + id (id in the tooltip for power users). The model id is also
        // stashed on Qt::UserRole so rebuildActionButtons / onDownloadRequested
        // can resolve the manifest entry after a refresh renumbers rows.
        auto* nameItem = new QTableWidgetItem(
            QString::fromStdString(e.name));
        nameItem->setToolTip(QString::fromStdString(
            std::string("id: ") + e.id + "\n" + e.description));
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        nameItem->setData(Qt::UserRole, QString::fromStdString(e.id));
        table_->setItem(row, kColName, nameItem);

        // Status (filled by setRowStatus).
        auto* statusItem = new QTableWidgetItem(QString());
        statusItem->setFlags(statusItem->flags() & ~Qt::ItemIsEditable);
        table_->setItem(row, kColStatus, statusItem);

        // Size.
        auto* sizeItem = new QTableWidgetItem(formatSize(e.sizeBytes));
        sizeItem->setFlags(sizeItem->flags() & ~Qt::ItemIsEditable);
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table_->setItem(row, kColSize, sizeItem);

        // License.
        const QString licText = e.license.empty()
            ? tr("？") : QString::fromStdString(e.license);
        auto* licItem = new QTableWidgetItem(licText);
        licItem->setFlags(licItem->flags() & ~Qt::ItemIsEditable);
        table_->setItem(row, kColLicense, licItem);

        // Action buttons (rebuilt by rebuildActionButtons).
        rebuildActionButtons(row);
        setRowStatus(row, statusFor(e));
    }
}

ModelManagementWidget::Status
ModelManagementWidget::statusFor(const lrc::ModelManifestEntry& e) const {
    // Per-row download state is tracked in activeHandles_ and patched in by
    // onDownloadRequested via setRowStatus(Downloading). This function only
    // reflects the on-disk / manifest state so the table is correct on first
    // render and after a refresh.
    if (e.downloadUrl.empty() && e.sizeBytes == 0)
        return Status::LicenseBlocked;
    if (mm_.isModelReady(e))
        return Status::Ready;
    return Status::NotDownloaded;
}

void ModelManagementWidget::setRowStatus(int row, Status s,
                                        const QString& detail) {
    auto* item = table_->item(row, kColStatus);
    if (!item) return;
    item->setText(statusText(s, row) + (detail.isEmpty() ? QString()
                                                          : QStringLiteral("  ") + detail));
    // Color-code the status text via the item's foreground brush.
    switch (s) {
        case Status::Ready:        item->setForeground(QColor("#1a7a1a")); break;
        case Status::NotDownloaded: item->setForeground(QColor("#666666")); break;
        case Status::LicenseBlocked:item->setForeground(QColor("#a06000")); break;
        case Status::Downloading:   item->setForeground(QColor("#0050a0")); break;
        case Status::Error:         item->setForeground(QColor("#a00000")); break;
    }
}

QString ModelManagementWidget::statusText(Status s, int /*row*/) const {
    switch (s) {
        case Status::Ready:         return tr("✓ 已就绪");
        case Status::NotDownloaded: return tr("未下载");
        case Status::LicenseBlocked:return tr("许可证受限");
        case Status::Downloading:   return tr("下载中");
        case Status::Error:         return tr("错误");
    }
    return {};
}

void ModelManagementWidget::rebuildActionButtons(int row) {
    auto* w = new QWidget(table_);
    auto* lay = new QHBoxLayout(w);
    lay->setContentsMargins(2, 0, 2, 0);
    lay->setSpacing(4);

    // Resolve the manifest entry from the model id stashed on the name item's
    // UserRole by refreshTable. A refresh renumbers rows, so we never index
    // the manifest by row directly.
    auto* nameItem = table_->item(row, kColName);
    const QString id = nameItem ? nameItem->data(Qt::UserRole).toString()
                                : QString();
    const auto entry = id.isEmpty()
        ? std::optional<lrc::ModelManifestEntry>{}
        : mm_.findModel(id.toStdString());
    if (!entry) {
        auto* lbl = new QLabel(tr("（已移除）"), w);
        lay->addWidget(lbl);
        lay->addStretch(1);
        table_->setCellWidget(row, kColAction, w);
        return;
    }

    const bool blocked = entry->downloadUrl.empty() && entry->sizeBytes == 0;
    const bool downloading = activeHandles_.contains(row);
    const bool ready = mm_.isModelReady(*entry);

    auto* downloadBtn = new QPushButton(
        downloading ? tr("取消") : (ready ? tr("重新下载") : tr("下载")), w);
    downloadBtn->setProperty("secondary", true);
    downloadBtn->setEnabled(!blocked);
    lay->addWidget(downloadBtn);

    auto* importBtn = new QPushButton(tr("导入文件…"), w);
    importBtn->setProperty("secondary", true);
    importBtn->setEnabled(!blocked);
    lay->addWidget(importBtn);

    lay->addStretch(1);

    // Re-wire: which slot the download button triggers depends on state.
    if (downloading) {
        connect(downloadBtn, &QPushButton::clicked, this, [this, row]() {
            onCancelRequested(row);
        });
    } else {
        connect(downloadBtn, &QPushButton::clicked, this, [this, row]() {
            onDownloadRequested(row);
        });
    }
    connect(importBtn, &QPushButton::clicked, this, [this, row]() {
        onImportRequested(row);
    });

    table_->setCellWidget(row, kColAction, w);
}

void ModelManagementWidget::onDownloadRequested(int row) {
    auto* nameItem = table_->item(row, kColName);
    if (!nameItem) return;
    const QString id = nameItem->data(Qt::UserRole).toString();
    auto entry = mm_.findModel(id.toStdString());
    if (!entry) return;

    // Reset UI for this download.
    progressBar_->setVisible(true);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    statusLabel_->setText(tr("正在下载 %1…").arg(
        QString::fromStdString(entry->name)));
    rowErrors_.remove(row);

    const quint64 h = mm_.downloadModel(*entry,
        [this, row, id](uint64_t recv, uint64_t total, bool finished,
                        const std::string& err) {
            // This callback fires on the Qt event loop thread (QNAM is async
            // and lives on the same thread as the dialog). Safe to touch UI.
            if (finished) {
                activeHandles_.remove(row);
                if (!err.empty()) {
                    rowErrors_[row] = QString::fromStdString(err);
                    setRowStatus(row, Status::Error,
                                 QString::fromStdString(err));
                    statusLabel_->setText(
                        tr("下载失败：%1").arg(QString::fromStdString(err)));
                } else {
                    setRowStatus(row, Status::Ready);
                    statusLabel_->setText(tr("下载完成：%1").arg(
                        QString::fromStdString(
                            mm_.findModel(id.toStdString())
                                ? mm_.findModel(id.toStdString())->name
                                : id.toStdString())));
                }
                progressBar_->setVisible(false);
                rebuildActionButtons(row);
                return;
            }
            if (total > 0) {
                const int pct = static_cast<int>((recv * 100) / total);
                progressBar_->setValue(pct);
                setRowStatus(row, Status::Downloading,
                             QString::number(pct) + QStringLiteral("%"));
            } else {
                setRowStatus(row, Status::Downloading, tr("连接中…"));
            }
        });
    if (h == 0) {
        statusLabel_->setText(tr("无法启动下载：%1").arg(
            QString::fromStdString(mm_.lastError())));
        progressBar_->setVisible(false);
        return;
    }
    activeHandles_[row] = h;
    setRowStatus(row, Status::Downloading, tr("连接中…"));
    rebuildActionButtons(row);
}

void ModelManagementWidget::onCancelRequested(int row) {
    const auto it = activeHandles_.constFind(row);
    if (it == activeHandles_.constEnd()) return;
    mm_.cancelDownload(*it);
    activeHandles_.remove(row);
    setRowStatus(row, Status::NotDownloaded, tr("已取消"));
    statusLabel_->setText(tr("已取消下载。"));
    progressBar_->setVisible(false);
    rebuildActionButtons(row);
}

void ModelManagementWidget::onImportRequested(int row) {
    auto* nameItem = table_->item(row, kColName);
    if (!nameItem) return;
    const QString id = nameItem->data(Qt::UserRole).toString();
    auto entry = mm_.findModel(id.toStdString());
    if (!entry) return;

    const QString filter = tr("ONNX 模型 (*.onnx);;ZIP 压缩包 (*.zip);;"
                              "所有文件 (*)");
    const QString src = QFileDialog::getOpenFileName(
        this, tr("选择 %1 的本地模型文件").arg(
            QString::fromStdString(entry->name)),
        QString(), filter);
    if (src.isEmpty()) return;

    const auto targetPath = mm_.cachePathFor(*entry);
    std::error_code ec;
    std::filesystem::create_directories(targetPath.parent_path(), ec);

    statusLabel_->setText(tr("正在导入 %1…").arg(
        QString::fromStdString(entry->name)));
    setRowStatus(row, Status::Downloading, tr("导入中…"));

    const QFileInfo fi(src);
    const bool isZip = fi.suffix().compare(QStringLiteral("zip"),
                                           Qt::CaseInsensitive) == 0;
    const auto srcPath = std::filesystem::path(src.toStdU16String());

    if (isZip) {
        std::string zipErr;
        if (!lrc::ModelManager::extractOnnxFromZip(srcPath, targetPath,
                                                   zipErr)) {
            rowErrors_[row] = QString::fromStdString(zipErr);
            setRowStatus(row, Status::Error,
                         QString::fromStdString(zipErr));
            statusLabel_->setText(
                tr("ZIP 解压失败：%1").arg(QString::fromStdString(zipErr)));
            return;
        }
    } else {
        // Bare .onnx import: copy into the cache path. If the source already
        // lives at the target (re-import), skip the copy.
        if (std::filesystem::weakly_canonical(srcPath) !=
            std::filesystem::weakly_canonical(targetPath)) {
            std::filesystem::copy_file(
                srcPath, targetPath,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                rowErrors_[row] = QString::fromStdString(ec.message());
                setRowStatus(row, Status::Error,
                             QString::fromStdString(ec.message()));
                statusLabel_->setText(
                    tr("复制失败：%1").arg(QString::fromStdString(ec.message())));
                return;
            }
        }
    }

    // Verify the imported file (SHA / existence).
    if (mm_.isModelReady(*entry)) {
        setRowStatus(row, Status::Ready);
        statusLabel_->setText(tr("导入完成：%1").arg(
            QString::fromStdString(entry->name)));
    } else {
        // File exists but SHA mismatch — common when the user imports a
        // different build of the same model. Surface the mismatch rather than
        // silently accepting.
        const auto actual = lrc::ModelManager::sha256File(targetPath);
        if (std::filesystem::exists(targetPath)) {
            const QString msg = tr("SHA256 不匹配（期望 %1，实际 %2）")
                .arg(QString::fromStdString(entry->sha256),
                     QString::fromStdString(actual));
            rowErrors_[row] = msg;
            setRowStatus(row, Status::Error, tr("SHA 不匹配"));
            statusLabel_->setText(msg);
        } else {
            rowErrors_[row] = tr("导入未产生目标文件");
            setRowStatus(row, Status::Error, tr("导入失败"));
            statusLabel_->setText(rowErrors_[row]);
        }
    }
    rebuildActionButtons(row);
}

void ModelManagementWidget::onOpenCacheFolder() {
    const auto root = lrc::ModelManager::cacheRoot();
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    QDesktopServices::openUrl(QUrl::fromLocalFile(
        QString::fromLocal8Bit(root.string())));
}

void ModelManagementWidget::reject() {
    // Abort any in-flight downloads before closing (QNAM destruction aborts
    // replies, but we cancel explicitly so the progress callback sees a clean
    // finish rather than mid-flight abort noise).
    for (auto it = activeHandles_.constBegin();
         it != activeHandles_.constEnd(); ++it) {
        mm_.cancelDownload(*it);
    }
    activeHandles_.clear();
    QDialog::reject();
}

QString ModelManagementWidget::formatSize(uint64_t bytes) const {
    if (bytes == 0) return QStringLiteral("—");
    constexpr double KiB = 1024.0, MiB = 1048576.0, GiB = 1073741824.0;
    if (bytes >= GiB) return QString::number(bytes / GiB, 'f', 2) + " GiB";
    if (bytes >= MiB) return QString::number(bytes / MiB, 'f', 1) + " MiB";
    if (bytes >= KiB) return QString::number(bytes / KiB, 'f', 0) + " KiB";
    return QString::number(bytes) + " B";
}
