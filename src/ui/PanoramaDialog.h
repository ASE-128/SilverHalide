#pragma once

/**
 * @brief Panorama merge dialog.
 *
 * Shows the ordered source thumbnails (2-20), lets the user choose the
 * projection mode (cylindrical vs pure-translation mosaic), auto-crop and the
 * focal-length estimate, then runs lrc::stitchPanorama on a background
 * PanoramaWorker (separate QThread) so the UI never blocks.
 *
 * On success the dialog emits mergedReady() with the stitched buffer and the
 * applied options; MainWindow is responsible for writing the DNG/TIFF and
 * importing it into the catalog — this dialog never writes files and never
 * touches the catalog.
 *
 * Progress: the engine call is a single monolithic stitchPanorama() with no
 * progress callback, so the progress bar is a busy indicator and the stage
 * label is a coarse UI-side approximation (aligning -> blending).
 *
 * Cancel: best-effort. A cancel takes effect at the worker's safe points
 * (before the stitch starts, or after it returns, discarding the result). The
 * dialog stays open while a stitch runs and closes once the worker reports a
 * terminal state.
 */

#include <vector>

#include <QDialog>
#include <QString>

#include "engine/ImageBuffer.h"
#include "engine/PanoramaStitcher.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QHBoxLayout;
class QLabel;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QTimer;
class QThread;
class PanoramaWorker;

class PanoramaDialog : public QDialog {
    Q_OBJECT
public:
    explicit PanoramaDialog(QWidget* parent = nullptr);
    ~PanoramaDialog() override;

    // Accepts the decoded, ordered source buffers. Rebuilds the thumbnail strip;
    // "Start merge" is enabled only for 2-20 sources.
    void setSources(const std::vector<lrc::ImageBufferPtr>& sources);

    // The PanoramaOptions applied for the most recent merge; valid after a
    // successful merge, default-constructed before any.
    const lrc::PanoramaOptions& options() const { return opts_; }

signals:
    // Stitched panorama. MainWindow writes the file and imports it; the dialog
    // does neither. Read the applied options with options() after exec() returns.
    void mergedReady(const lrc::ImageBufferPtr& result);
    // The user cancelled (idle close, or a cancel during the stitch took effect
    // at a safe point). No result is delivered.
    void cancelled();

protected:
    // Intercept close/escape so the dialog stays open while a stitch runs and
    // only closes after the worker reaches a safe point.
    void reject() override;

private slots:
    void onStartClicked();
    void onCancelClicked();
    void onProjectionChanged(int index);
    void onMergeFinished(const lrc::ImageBufferPtr& result);
    void onMergeFailed(const QString& message);
    void onMergeCancelled();
    void onStageTimer();

private:
    void buildUi();
    void rebuildThumbnails();
    void setBusy(bool busy);
    void stopThread();
    lrc::PanoramaOptions currentOptions() const;

    std::vector<lrc::ImageBufferPtr> sources_;
    lrc::PanoramaOptions opts_;

    QHBoxLayout* thumbLayout_ = nullptr;
    QScrollArea* thumbArea_ = nullptr;
    QLabel* hintLabel_ = nullptr;
    QComboBox* projectionCombo_ = nullptr;
    QCheckBox* autoCropCheck_ = nullptr;
    QDoubleSpinBox* focalSpin_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QLabel* stageLabel_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QTimer* stageTimer_ = nullptr;

    QThread* thread_ = nullptr;
    PanoramaWorker* worker_ = nullptr;
};
