#pragma once

/**
 * @brief Named-develop-preset panel (P3-3 preset management).
 *
 * Wraps the on-disk PresetLibrary with a compact UI: save the current
 * adjustment stack under a name, list the stored presets, apply one, delete
 * one, or export one to an arbitrary file location for sharing. Applying
 * emits `applied` with the preset's adjustment stack; MainWindow routes that
 * through commitAdjustments so an apply is undoable. The dialog does NOT own
 * the library (callers keep it alive), matching SnapshotDialog's ownership
 * pattern.
 */

#include <QDialog>
#include <QString>

#include "engine/Adjustments.h"
// applied() carries lrc::Adjustments, so its metatype must be declared before
// moc's generated code uses it.
#include "ui/MetaTypes.h"

class QLineEdit;
class QListWidget;
class QListWidgetItem;

namespace lrc {
class PresetLibrary;
} // namespace lrc

class PresetDialog : public QDialog {
    Q_OBJECT
public:
    /**
     * @brief Builds the preset panel bound to `library`.
     *
     * @param library Preset store to read/write; must outlive the dialog.
     * @param current Adjustment stack captured at open time, saved verbatim
     *                when the user creates a new preset.
     * @param parent  Optional owner widget.
     */
    PresetDialog(lrc::PresetLibrary& library,
                 const lrc::Adjustments& current, QWidget* parent = nullptr);

public slots:
    /** @brief Re-queries the library and rebuilds the list. */
    void reload();

signals:
    /**
     * @brief Emitted when the user applies a listed preset.
     *
     * MainWindow commits it through commitAdjustments (undoable) and closes
     * the dialog.
     */
    void applied(const lrc::Adjustments& adjustments);

private slots:
    void onCreateClicked();
    void onApplyClicked();
    void onDeleteClicked();
    void onExportClicked();

private:
    void okMessage(const QString& text);
    // Currently selected preset name (item UserRole), or empty when the list
    // has no selection.
    QString selectedName() const;

    lrc::PresetLibrary& library_;
    lrc::Adjustments current_;
    QListWidget* list_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
};
