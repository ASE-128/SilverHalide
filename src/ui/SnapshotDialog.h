#pragma once

/**
 * @brief Named-edit-snapshot panel.
 *
 * Wraps the catalog's snapshot API (saveEditSnapshot / listEditHistory /
 * loadEditSnapshot / deleteEditSnapshot / renameSnapshot) with a compact UI:
 * create, apply, rename and delete named snapshots for the current
 * image/variant. Applying emits `applied` with the snapshot's adjustment stack;
 * MainWindow wires that back into the develop pipeline via
 * applyAdjustmentState().
 */

#include <QDialog>
#include <QString>

#include <cstdint>
#include <vector>

#include "engine/Adjustments.h"

class QLineEdit;
class QListWidget;
class QListWidgetItem;

namespace lrc {
class CatalogDatabase;
} // namespace lrc

class SnapshotDialog : public QDialog {
    Q_OBJECT
public:
    // `variantId` selects which virtual copy's snapshot list to work on
    // (0 = master), matching the rest of the edit-history API.
    SnapshotDialog(lrc::CatalogDatabase& catalog, qint64 imageId,
                   int variantId, QWidget* parent = nullptr);

public slots:
    void reload();

signals:
    // Emitted when the user clicks Apply for a snapshot. MainWindow applies the
    // returned stack (set to the develop pipeline + saved).
    void applied(const lrc::Adjustments& adjustments);

private slots:
    void onCreateClicked();
    void onApplyClicked();
    void onRenameClicked();
    void onDeleteClicked();

private:
    void okMessage(const QString& text);

    lrc::CatalogDatabase& catalog_;
    qint64 imageId_;
    int variantId_;
    QListWidget* list_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
};