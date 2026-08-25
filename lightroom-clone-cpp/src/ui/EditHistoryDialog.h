#pragma once

// Edit-history dialog: list every committed snapshot for one image and let the
// user restore a previous parameter stack. The dialog only loads/validates the
// chosen snapshot; MainWindow persists it (one undoable step, one fresh history
// snapshot) after the dialog is accepted.

#include <cstdint>
#include <optional>
#include <QDialog>

#include "engine/Adjustments.h"

namespace lrc {
class CatalogDatabase;
}

class QListWidget;
class QPushButton;

class EditHistoryDialog : public QDialog {
    Q_OBJECT
public:
    EditHistoryDialog(lrc::CatalogDatabase& catalog, int64_t imageId,
                      const QString& imageName, QWidget* parent = nullptr);
    // 用户点击「恢复」后返回被恢复的调整参数；取消或未选择时返回 nullopt.
    std::optional<lrc::Adjustments> restored() const;

private slots:
    void onRestore();

private:
    void refresh();
    lrc::CatalogDatabase& catalog_;
    int64_t imageId_;
    QListWidget* list_ = nullptr;
    QPushButton* restoreButton_ = nullptr;
    std::optional<lrc::Adjustments> restored_;
};
