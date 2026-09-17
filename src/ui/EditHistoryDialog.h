#pragma once

/**
 * @brief Edit-history dialog.
 *
 * Lists every committed snapshot for one image and lets the user restore a
 * previous parameter stack. The dialog only loads/validates the chosen
 * snapshot; MainWindow persists it (one undoable step, one fresh history
 * snapshot) after the dialog is accepted.
 */

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
    // Returns the restored adjustment parameters after the user clicks Restore;
    // nullopt when cancelled or nothing was selected.
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
