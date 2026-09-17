#pragma once

/**
 * @brief Lightroom-style import dialog.
 *
 * Picks any importable photo (RAW, DNG, JPEG, PNG, TIFF) and offers three
 * import policies — Add / Copy / Move — plus an "import as DNG" checkbox. When
 * enabled, each imported RAW (ARW/SR2/...) is first converted to a .dng and the
 * DNG is cataloged instead. Display-ready formats pass through unchanged.
 * MainWindow routes the chosen paths + policy + destination to
 * ImportWorker::importPaths.
 */

#include <QDialog>
#include <QStringList>

#include "catalog/LibraryManager.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

class ImportDialog : public QDialog {
    Q_OBJECT
public:
    explicit ImportDialog(const QString& defaultDestDir, QWidget* parent);
    explicit ImportDialog(QWidget* parent = nullptr);

    QStringList selectedPaths() const;
    bool convertToDng() const;
    lrc::ImportMode importMode() const;
    // Target directory for Copy/Move (may be empty).
    QString destinationDir() const;
    // True when the user switched on "rename on import" and typed a template.
    bool renameEnabled() const;
    // The rename template text ({date}/{index}/{base}) or empty when disabled.
    QString renameTemplate() const;

private slots:
    void browse();
    void browseDest();
    void refreshMode();
    void validateAndAccept();
    void refreshRenamePreview();

private:
    void refreshSummary();

    QPushButton* browseButton_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QCheckBox* convertBox_ = nullptr;
    QRadioButton* addRadio_ = nullptr;
    QRadioButton* copyRadio_ = nullptr;
    QRadioButton* moveRadio_ = nullptr;
    QLineEdit* destEdit_ = nullptr;
    QPushButton* browseDestButton_ = nullptr;
    QCheckBox* renameBox_ = nullptr;
    QLineEdit* renameEdit_ = nullptr;
    QLabel* renamePreview_ = nullptr;
    QStringList paths_;
};
