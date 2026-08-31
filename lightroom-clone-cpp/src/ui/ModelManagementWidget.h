#pragma once

// Settings dialog for AI model management. Lists every model declared in
// resources/models/manifest.json, shows its on-disk readiness (SHA-verified /
// not downloaded / license-blocked), and lets the user:
//   • Download  — async fetch via ModelManager::downloadModel (QNAM), with a
//                 progress bar and a per-row "取消" affordance. ZIP-packaged
//                 models (nafnet-denoise) are auto-extracted on completion.
//   • 导入文件  — pick a local .onnx (or .zip) and place it at the cache path,
//                 for offline / air-gapped setups.
//   • 打开缓存目录 — reveal %LOCALAPPDATA%/Silver Halide/models/ in Explorer.
//
// Launched from MainWindow's "设置 → 模型管理…" menu. Owns its own
// ModelManager instance; in-flight downloads are aborted when the dialog
// closes (QNAM destruction aborts replies, same lifecycle the nafnet_smoke
// test relies on).

#include <QDialog>
#include <QHash>

#include "app/ModelManager.h"

class QProgressBar;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;
class QLabel;

class ModelManagementWidget : public QDialog {
    Q_OBJECT
public:
    explicit ModelManagementWidget(QWidget* parent = nullptr);

private slots:
    void refreshTable();
    void onDownloadRequested(int row);
    void onImportRequested(int row);
    void onCancelRequested(int row);
    void onOpenCacheFolder();
    void reject() override;

private:
    enum class Status { Ready, NotDownloaded, LicenseBlocked, Downloading, Error };
    Status statusFor(const lrc::ModelManifestEntry& e) const;
    QString statusText(Status s, int row) const;
    void setRowStatus(int row, Status s, const QString& detail = {});
    void rebuildActionButtons(int row);
    QString formatSize(uint64_t bytes) const;

    lrc::ModelManager mm_;
    QTableWidget* table_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    // row -> active download handle (0 = none). One download per row at a time.
    QHash<int, quint64> activeHandles_;
    // row -> last error message (for the status cell tooltip).
    QHash<int, QString> rowErrors_;
};
