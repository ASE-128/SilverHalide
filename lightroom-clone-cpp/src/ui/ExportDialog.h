#pragma once

// Multi-format export dialog (JPEG / PNG / TIFF-8 / TIFF-16 / DNG). The engine
// layer (ExportEngine) already supports every format; this dialog gathers the
// format-specific options and an output path, then MainWindow routes the job
// to PreviewWorker::exportImage.

#include <QDialog>
#include <QImage>
#include <QString>

#include <vector>

#include "ui/PreviewWorker.h"  // ExportWatermark (P1-4)

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSlider;
class QWidget;

namespace lrc {

// catalog/CatalogDatabase.h (M6). Forward-declared so this header stays
// Qt-only and free of catalog/ includes.
struct EditVariant;

} // namespace lrc

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    // `suggestedPath` is the default output path shown in the path field.
    explicit ExportDialog(const QString& suggestedPath, QWidget* parent = nullptr);

    QString outputPath() const;
    // Combo index == PreviewWorker::ExportFormat value.
    int formatIndex() const;
    int jpegQuality() const;
    // S17-18: longest-edge pixel cap for the exported image. 0 = keep native
    // resolution (no downscale). Read from the size-preset combo.
    int maxDim() const;

    // --- virtual-copy / variant selection (M6) -----------------------------
    // Fills the variant selector with the master plus every virtual copy of
    // the current photo. `variants` is expected to be the result of
    // CatalogDatabase::listVariants(imageId). The selector is shown only when
    // the list contains at least one virtual copy (non-master); otherwise it
    // stays hidden and the legacy behaviour is unchanged (exports always target
    // the master, variant 0).
    void setVariants(const std::vector<lrc::EditVariant>& variants);
    // True when setVariants() supplied at least one virtual copy, i.e. the
    // selector is active.
    bool hasVariants() const;
    // variantId of the selected variant; 0 (master) when no virtual copies
    // exist, matching the legacy export path.
    qint64 selectedVariantId() const;

    // Composed a suggested filename for the given format index (incl. dot).
    static QString defaultFileName(const QString& baseName, int formatIndex);

    // --- P1-2 ICC ----------------------------------------------------------
    // Returns an ExportEngine::ExportSpec::IccChoice value (0 = none).
    int iccIndex() const;
    // --- P1-4 watermark -----------------------------------------------------
    // Returns the configured watermark (RGBA overlay + position/opacity/scale).
    ExportWatermark watermark() const;

    // --- P1-3 batch rename --------------------------------------------------
    // True when the user switched on "rename on batch export" and typed a
    // template. When enabled, MainWindow exports the master + all virtual
    // copies into the chosen folder, naming each output from the template
    // instead of the single-variant legacy path.
    bool renameEnabled() const;
    // The rename template text ({date}/{index}/{base}) or empty when disabled.
    QString renameTemplate() const;

signals:
    // Emitted when the user picks a different variant in the selector. Not
    // emitted while setVariants() is populating the list.
    void variantChosen(qint64 variantId);

private slots:
    void browse();
    void onFormatChanged(int index);
    void validateAndAccept();
    void browseWatermark();
    void onWatermarkToggle(bool on);
    void refreshRenamePreview();

private:
    void updatePathSuffix();

    QComboBox* variantCombo_ = nullptr;
    QComboBox* formatCombo_ = nullptr;
    QComboBox* sizeCombo_ = nullptr;  // S17-18: longest-edge preset.
    QComboBox* iccCombo_ = nullptr;   // P1-2: ICC preset (0 = none).
    QCheckBox* renameBox_ = nullptr;  // P1-3: batch-rename on/off.
    QLineEdit* renameEdit_ = nullptr; // P1-3: rename template.
    QLabel* renamePreview_ = nullptr; // P1-3: rendered batch-name preview.
    // P1-3: cached variant list (master + virtual copies) so the preview can
    // show the batch outputs without re-querying the catalog.
    std::vector<lrc::EditVariant> variants_;
    QCheckBox* wmEnable_ = nullptr;   // P1-4: watermark on/off.
    QLabel* wmPathLabel_ = nullptr;   // P1-4: chosen overlay file label.
    QComboBox* wmPosition_ = nullptr; // P1-4: corner / centre.
    QSlider* wmOpacity_ = nullptr;    // P1-4: opacity 0..100.
    QSlider* wmScale_ = nullptr;      // P1-4: scale 0..50 (~0..0.5 edge ratio).
    QWidget* wmRow_ = nullptr;        // P1-4: watermark options group.
    // P1-4: straight-alpha RGBA overlay decoded from the chosen PNG.
    QImage wmOverlay_;

    QLineEdit* pathEdit_ = nullptr;
    QWidget* qualityRow_ = nullptr;
    QSlider* qualitySlider_ = nullptr;
    QLabel* qualityValueLabel_ = nullptr;

    // The extension currently appended to the path field (e.g. ".jpg").
    QString currentSuffix_;
    // Base path without extension; kept when the format changes.
    QString basePath_;
    // True once the user has typed into the path field; guards onFormatChanged
    // from refilling a path the user deliberately cleared.
    bool pathUserEdited_ = false;
    // True when setVariants() found at least one virtual copy (non-master), so
    // the selector is active. Mirrors the combo visibility without depending on
    // the widget being shown yet.
    bool variantsAvailable_ = false;
};
