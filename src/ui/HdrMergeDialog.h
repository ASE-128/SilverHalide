#pragma once

// HDR merge dialog: pick 2+ bracketed photos from the catalog (checkboxes),
// optionally enable de-ghosting, choose the output DNG path, then MainWindow
// routes the selection to PreviewWorker::mergeHdr on the background thread.

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QVector>

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;

struct HdrMergeImage {
    int id = 0;
    QString fileName;
    QString filePath;
};

class HdrMergeDialog : public QDialog {
    Q_OBJECT
public:
    explicit HdrMergeDialog(const QVector<HdrMergeImage>& images,
                            QWidget* parent = nullptr);

    QStringList selectedPaths() const;
    bool deghost() const;
    QString outputPath() const;

private slots:
    void browse();
    void onSelectionChanged();
    void validateAndAccept();

private:
    QListWidget* list_ = nullptr;
    QCheckBox* deghostCheck_ = nullptr;
    QLineEdit* pathEdit_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
};
