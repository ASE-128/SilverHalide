#pragma once

// PHASE_15: Soft Proofing dialog.
//
// Modal dialog wrapping engine/SoftProofing.h.  The user picks:
//   * destination profile (dropdown of installed ICC profiles plus "Load from
//     file…")
//   * rendering intent (Perceptual / Relative Colorimetric / Absolute
//     Colorimetric / Saturation — maps directly to lcms2 intents)
//   * use black-point compensation  (on by default; the SoftProofing engine
//     uses the cmsFLAGS_BLACKPOINTCOMPENSATION flag when true)
//   * simulate paper & ink (gamut warning + PCS black-point mapping for
//     Absolute Colorimetric — SoftProofing handles it via the paperWhite flag)
//   * out-of-gamut warning color + toggle
//
// Live preview: the dialog owns a small SoftProofing instance and exposes
// applyProof() as slot that MainWindow can invoke on the currently-displayed
// preview QImage.  Because preview size changes (fit-zoom etc.) are handled
// by the image view, the dialog exposes *parameters only* and the actual
// rendering happens inside ImageViewport — this header only packages the
// controls.

#include <QColor>
#include <QDialog>
#include <QString>

#include "engine/SoftProofing.h"

class QComboBox;
class QCheckBox;
class QPushButton;
class QLabel;
class ColorManager;

namespace lrc {

class SoftProofingDialog : public QDialog {
    Q_OBJECT
public:
    explicit SoftProofingDialog(ColorManager* cm, QWidget* parent = nullptr);

    // Returns a snapshot of the selected proof parameters so the caller
    // (MainWindow / ImageViewport) can apply them to the current preview.
    SoftProofParams currentParams() const;
    bool proofingEnabled() const;

signals:
    // Emitted whenever any parameter changes.  MainWindow repaints the
    // current viewport preview through the SoftProofing engine's applyProof.
    void paramsChanged(const SoftProofParams& p, bool enabled);
    // Emitted when user presses "Export sRGB gamut report PNG" — MainWindow
    // writes the file and pops a success banner.
    void exportGamutReportRequested();

private slots:
    void emitChanged();
    void onLoadProfileFromFile();
    void onHighlightColor();
    void onExportReport();

private:
    void populateIntentCombo();
    void populateProfileCombo();

    ColorManager* cm_ = nullptr;
    QComboBox* profileCombo_ = nullptr;
    QComboBox* intentCombo_ = nullptr;
    QCheckBox* bpcCheck_ = nullptr;
    QCheckBox* paperCheck_ = nullptr;
    QCheckBox* highlightCheck_ = nullptr;
    QPushButton* highlightColorBtn_ = nullptr;
    QCheckBox* enableCheck_ = nullptr;
    QLabel* previewLabel_ = nullptr;
    QPushButton* exportBtn_ = nullptr;
    QPushButton* closeBtn_ = nullptr;

    QColor highlightColor_ = QColor(255, 0, 220); // magenta default
    QString customProfilePath_; // non-empty when user loaded a file explicitly
};

} // namespace lrc
