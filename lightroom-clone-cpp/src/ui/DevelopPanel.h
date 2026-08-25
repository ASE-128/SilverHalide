#pragma once

// Right-hand develop panel. Lightroom-style collapsible sections (基本 /
// 色调曲线 / 细节 / 颜色 / 色调分离 / 效果 / 镜头校正) each holding one row per
// adjustment (label + slider + spin box). Slider movement is debounced (50 ms)
// before the pipeline re-renders; a committed signal fires on release so the
// catalog can persist the stack without hammering the disk during a drag.

#include <QDoubleSpinBox>
#include <QTimer>
#include <QWidget>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/Adjustments.h"
// Full PhotoMetadata definition is required by moc's qt_metacall for the
// setMetadata slot (it dereferences the argument), so it must be complete in
// this header, not just forward-declared. catalog/ is Qt-free, so pulling it
// into a UI header is a one-way dependency (UI -> catalog).
#include "catalog/CatalogDatabase.h"

class ColorWheelWidget;
class CurveEditor;
class HistogramWidget;
class PanelSection;
class QCheckBox;
class QColor;
class QComboBox;
class QDoubleSpinBox;
class QEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QTabWidget;
class QVBoxLayout;

class DevelopPanel : public QWidget {
    Q_OBJECT
public:
    explicit DevelopPanel(QWidget* parent = nullptr);

    lrc::Adjustments adjustments() const { return current_; }
    void setAdjustments(const lrc::Adjustments& adjustments);
    void resetAll();

    // The embedded histogram (kept in sync with the preview by MainWindow).
    HistogramWidget* histogram() const { return histogram_; }

    // Sets the sampler button's checked state without emitting.
    // (Must NOT be a signal — moc would generate a duplicate definition.)
    void setSamplerChecked(bool active);

    // Disarms the active mask tool (unchecks the buttons, deselects the mask
    // combo). Called by MainWindow when the image changes.
    void clearMaskTool();

    // The user changed the geometry of `index` in the viewport; the panel
    // copies the geometry (type/linear/radial/brush/invert) but preserves the
    // local adjustment values, then re-renders and persists.
    void updateMaskGeometry(int index, const lrc::Mask& mask);

    // --- point-colour (ColorTarget) panel -----------------------------------
    // Records the reference colour sampled from the image for target `index`
    // (index < 0 creates a new target, LR-style). `displayColor` is the sRGB
    // preview colour shown in the swatch; r/g/b are the linear-RGB reference.
    void setColorTargetReference(int index, const QColor& displayColor, float r,
                                 float g, float b);
    // Index of the currently selected point-colour target (-1 if none).
    int currentColorTargetIndex() const;

    // --- spot removal (M5) --------------------------------------------------
    // Current spot tool mode / radius (normalized 0.001..0.2 against the longer
    // image edge). Lets MainWindow initialize the viewport's tool state from
    // the panel defaults on startup.
    lrc::SpotType spotMode() const { return spotMode_; }
    float spotRadius() const {
        return spotRadiusSpin_ ? static_cast<float>(spotRadiusSpin_->value())
                               : 0.01f;
    }
    // Sets the reference-sampler button's checked state without emitting.
    // (Must NOT be a signal — moc would generate a duplicate definition.)
    void setColorTargetSamplerChecked(bool active);

    // direction > 0 增大 10%,< 0 减小 10%(相对当前 brush size 乘/除 1.1).
    // 无蒙版工具激活时 no-op. MainWindow 从快捷键 [ / ] 调用.
    void changeMaskBrushSize(int direction);
    // 曝光微调:deltaEv=+0.33 或 -0.33(0.33 档). MainWindow 从 +/- 快捷键调用.
    void adjustExposure(double deltaEv);

    // --- colour-range mask panel --------------------------------------------
    // Records the sRGB reference colour sampled from the image for the
    // colour-range mask; MainWindow calls this after the viewport's sampler
    // samples. Updates the swatch and enables the tolerance row. (Plain method,
    // NOT a signal -- moc would generate a duplicate definition.)
    void setColorRangeReference(const QColor& srgb);

    // --- range sub-mask panel (PHASE_4) --------------------------------------
    // Records the reference colour sampled from the image for the *colour-range
    // sub-mask* of the currently selected geometry mask. `srgb` is the display
    // colour (swatch); `lr/lg/lb` are the linear ProPhoto RGB reference written
    // into the mask. Enables useColorRange, writes colorRefR/G/B, re-renders
    // and persists. (Plain method -- moc would duplicate a signal with this
    // name.)
    void setColorRangeSubReference(const QColor& srgb, float lr, float lg,
                                   float lb);
    // Records the relative luminance sampled from the image (0-100 percent,
    // linear Rec.709 luma) for the *luminance-range sub-mask* of the currently
    // selected geometry mask. Enables useLuminance and writes luminanceRef.
    void setLuminanceSubReference(double luma);

    // Sets the range sub-mask sampler buttons' checked state without emitting
    // (used by MainWindow's C/W global-key handlers to keep the panel buttons
    // in sync when the viewport sampler is toggled directly). (Must NOT be a
    // signal -- moc would generate a duplicate definition.)
    void setRangeSubSamplerChecked(bool active);

public slots:
    // 公开槽:选中蒙版工具(线性/径向/画笔)或 None 禁用. 语义与面板按钮一致:
    // 勾选对应按钮、设 activeMaskTool_,并按现有逻辑 emit maskEditRequested.
    // MainWindow 从快捷键 K/M/Shift+M 及(未来)工具栏调用.
    void selectMaskTool(lrc::MaskType tool);

    // --- spot removal (M5) --------------------------------------------------
    // MainWindow mirrors the viewport's current spot radius (normalized
    // 0.001..0.2) back into the panel so the slider tracks keyboard / wheel
    // changes made outside the panel. Never emits spotRadiusChanged.
    void setSpotRadius(float radius);
    // MainWindow mirrors the viewport's current spot count so the panel can
    // show how many corrections exist (e.g. "污点数: 3").
    void setCurrentSpotCount(int count);
    // --- metadata (M8) ------------------------------------------------------
    // MainWindow populates the panel when switching photos. `keywordNames` are
    // the photo's keyword full paths ("风光/冰岛"); the panel shows them as
    // removable chips and echoes them back in metadataEdited.
    void setMetadata(const lrc::PhotoMetadata& metadata,
                     const std::vector<std::string>& keywordNames);

signals:
    // Fired (debounced) while dragging — drives the live preview.
    void adjustmentsChanged(const lrc::Adjustments& adjustments);
    // Fired (coarse-debounced, 30 ms) while a slider is dragged. Drives the
    // low-resolution live preview; adjustmentsChanged (refine) follows on pause
    // or release.
    void adjustmentsChangedCoarse(const lrc::Adjustments& adjustments);
    // Fired once on release / spin commit — drives persistence.
    void adjustmentsCommitted(const lrc::Adjustments& adjustments);
    // User asked for the pipeline auto-adjust suggestion (MainWindow computes
    // it from the current linear image and calls setAdjustments back).
    void autoRequested();
    // User toggled the point-colour sampler (MainWindow routes it to the
    // viewport's sampler tool). `active` is the new button state.
    void colorSamplerRequested(bool active);
    // User toggled the point-colour reference sampler (MainWindow routes it to
    // the viewport's colour-target sampler tool, which calls
    // setColorTargetReference back). `active` is the new button state.
    void colorTargetSamplerRequested(bool active);

    // LR-style Alt-key visualization: emitted (debounced) while the user holds
    // Alt and drags a slider whose adjustment has a diagnostic view. `mode` is
    // Pipeline::VisualizeMode as an int. Cleared when Alt is released.
    void visualizationRequested(int mode, const lrc::Adjustments& adjustments);
    void visualizationCleared();

    // --- Masked local adjustments ------------------------------------------
    // The user armed a mask tool / created / selected a mask. MainWindow routes
    // it to the viewport (tool + draft + red overlay). A mask with type None
    // and index -1 disables the tool.
    void maskEditRequested(const lrc::Mask& mask, int index);
    // Brush size slider (0..100) -> normalized dab radius (MainWindow routes it
    // to ImageViewport::setMaskBrushSize).
    void maskBrushSizeChanged(double normalizedRadius);
    // Brush softness slider (0..10000) -> normalized softness 0..1 (MainWindow
    // routes it to ImageViewport::setMaskBrushSoftness).
    void maskSoftnessChanged(double softness);
    // The active mask tool changed: emitted after any activation (panel tool
    // buttons, the selectMaskTool slot from shortcuts/toolbar, or picking an
    // existing mask in the combo) and after any path that disarms it to None.
    // MainWindow mirrors it in its toolbar (the panel is the source).
    void maskToolActivated(lrc::MaskType tool);

    // User toggled the colour-range sampler button (MainWindow routes it to the
    // viewport's colour-range sampler tool, which calls setColorRangeReference
    // back with the sampled colour). `active` is the new button state.
    void colorRangeSamplerRequested(bool active);

    // User toggled the colour-range *sub-mask* sampler button (MainWindow
    // routes it to the viewport's colour-range sampler tool, which calls
    // setColorRangeSubReference back with the sampled colour).
    void colorRangeSubSamplerRequested(bool active);

    // User toggled the luminance-range *sub-mask* sampler button (MainWindow
    // routes it to the viewport's luminance sampler tool, which calls
    // setLuminanceSubReference back with the sampled relative luminance).
    void luminanceSamplerRequested(bool active);

    // 用户开始一次滑块拖动手势(压栈起点),结束手势时发出 finished(MainWindow 收栈).
    void editGestureStarted();
    void editGestureFinished();

    // --- spot removal (M5) --------------------------------------------------
    // The active spot mode changed (Heal/Clone); MainWindow routes it to the
    // viewport for the next click.
    void spotModeChanged(lrc::SpotType type);
    // The spot radius changed (normalized 0.001..0.2 against the longer image
    // edge); MainWindow routes it to the viewport.
    void spotRadiusChanged(float radius);
    // User toggled spot visualization (invert + high-contrast preview); the
    // viewport switches its display mode.
    void visualizeSpotsToggled(bool active);

    // --- metadata (M8) ------------------------------------------------------
    // The user edited one of the metadata fields or the keyword list; MainWindow
    // persists the values via CatalogDatabase.
    void metadataEdited(const std::string& title, const std::string& caption,
                        const std::string& copyright, const std::string& creator,
                        const std::vector<std::string>& keywords);

private:
    // Which LR Alt-view a row can show; 0 = none.
    enum VizMode : uint8_t {
        VizNone = 0,
        VizClipping = 1,   // exposure / whites / blacks
        VizDetail = 2,     // clarity / dehaze / texture
        VizEdge = 3,       // sharpness masking
    };

    struct Row {
        const char* label;
        double min;
        double max;
        double decimals;
        double lrc::Adjustments::*field;
        uint8_t vizMode = VizNone;
        QSlider* slider = nullptr;
        QDoubleSpinBox* spin = nullptr;
    };

    struct HSLRow {
        const char* label;
        double lrc::HSLChannel::*field;
        double min;
        double max;
        QSlider* slider = nullptr;
        QDoubleSpinBox* spin = nullptr;
    };

    struct SplitToneRow {
        const char* label;
        double lrc::SplitTone::*field;
        double min;
        double max;
        QSlider* slider = nullptr;
        QDoubleSpinBox* spin = nullptr;
    };

    struct MaskRow {
        const char* label;
        double lrc::Mask::*field;
        double min;
        double max;
        QSlider* slider = nullptr;
        QDoubleSpinBox* spin = nullptr;
    };

    struct ColorTargetRow {
        const char* label;
        double lrc::ColorTarget::*field;
        double min;
        double max;
        QSlider* slider = nullptr;
        QDoubleSpinBox* spin = nullptr;
    };

    void addRow(Row row);
    void addHSLRow(int colorIndex, HSLRow row);
    void addSplitToneRow(SplitToneRow row);
    void addMaskRow(MaskRow row);
    void onSliderChanged(const Row& row, int value);
    void onSpinChanged(const Row& row, double value);
    void requestAltVisualization(const Row& row);
    void clearAltVisualization();
    void onHSLSliderChanged(int colorIndex, const HSLRow& row, int value);
    void onHSLSpinChanged(int colorIndex, const HSLRow& row, double value);
    void onSplitToneSliderChanged(const SplitToneRow& row, int value);
    void onSplitToneSpinChanged(const SplitToneRow& row, double value);
    void onMaskSliderChanged(const MaskRow& row, int value);
    void onMaskSpinChanged(const MaskRow& row, double value);
    void onMaskToolToggled(lrc::MaskType tool, bool checked);
    void onMaskSelected(int index);
    void onMaskDelete();
    void onMaskOpacityChanged(int value);
    void onMaskInvertToggled(bool checked);
    void onMaskFeatherChanged(int value);
    void onMaskBrushSizeChanged(int value);
    void onMaskSoftnessChanged(int value);
    void scheduleDebounce();
    void applyDebounce();
    void applyCoarseDebounce();
    // M11: coalesces the committed-geometry emission while a mask/brush stroke
    // is in flight so a stroke does not trigger a DB write + full panel re-sync
    // per mouse move (see updateMaskGeometry).
    void scheduleMaskGeometryCommit();
    void flushMaskGeometryCommit();
    void syncRow(const Row& row);
    void syncHSLRow(int colorIndex, const HSLRow& row);
    void syncSplitToneRow(const SplitToneRow& row);
    void syncMaskRow(const MaskRow& row);
    void syncMaskSection();
    void syncMaskSliders();
    int currentMaskIndex() const;
    // Checks the tool button for `tool` without emitting onMaskToolToggled.
    void armMaskTool(lrc::MaskType tool);
    // Activates `tool` for real: creates a fresh mask, selects it, and emits
    // maskEditRequested. Shared by the button path and selectMaskTool.
    void activateMaskTool(lrc::MaskType tool);
    // Unchecks all mask tool buttons without emitting.
    void uncheckMaskTools();
    // Shows/hides the feather / brush-size rows and enables the delete button
    // for the currently selected mask's type.
    void updateMaskSectionVisibility();
    // Re-emits maskEditRequested for the current mask (after a geometry change
    // from the panel, e.g. invert / feather).
    void refreshMaskDraft();

    // --- colour-range mask ---------------------------------------------------
    void onColorRangeToleranceSlider(int value);
    void onColorRangeToleranceSpin(double value);
    void onColorRangeSampleToggled(bool active);
    void syncColorRangeSection();
    void updateColorRangeSwatch(const QColor& color);
    // True when `m` is a geometry mask (linear / radial / brush) that can carry
    // the PHASE_4 colour/luminance sub-mask modifiers.
    bool isGeometryMask(const lrc::Mask& m);

    // --- range sub-mask (colour / luminance) (PHASE_4) -----------------------
    void onSubColorEnableToggled(bool checked);
    void onSubColorRangeSlider(int value);
    void onSubColorRangeSpin(double value);
    void onSubColorSmoothSlider(int value);
    void onSubColorSmoothSpin(double value);
    void onSubColorSampleToggled(bool active);
    void onSubLumaEnableToggled(bool checked);
    void onSubLumaRangeSlider(int value);
    void onSubLumaRangeSpin(double value);
    void onSubLumaSmoothSlider(int value);
    void onSubLumaSmoothSpin(double value);
    void onSubLumaSampleToggled(bool active);
    void onSubUnionChanged(int index);
    void syncSubRangeSection();
    void updateSubColorSwatch(const QColor& color);
    void updateSubLumaValueLabel(double luma);

    // --- point-colour (ColorTarget) panel -----------------------------------
    void addColorTargetRow(ColorTargetRow row);
    // Connects sliderPressed/sliderReleased to the edit-gesture signals.
    void connectSliderGesture(QSlider* slider);
    void onColorTargetSelected(int index);
    void onColorTargetAdd();
    void onColorTargetDelete();
    void onColorTargetEnabledToggled(bool checked);
    void onColorTargetSliderChanged(const ColorTargetRow& row, int value);
    void onColorTargetSpinChanged(const ColorTargetRow& row, double value);
    void syncColorTargetRow(const ColorTargetRow& row);
    void syncColorTargetSection();
    void syncColorTargetSliders();
    void updateColorTargetSwatch(const QColor& color);

    // --- spot removal (M5) --------------------------------------------------
    void onSpotModeToggled(lrc::SpotType type, bool checked);
    void onSpotRadiusSliderChanged(int value);
    void onSpotRadiusSpinChanged(double value);
    void onSpotVisualizeToggled(bool active);
    void syncSpotRadius(double radius);
    // --- metadata (M8) ------------------------------------------------------
    bool eventFilter(QObject* watched, QEvent* event) override;
    void rebuildKeywordChips();
    void addKeyword(const QString& text);
    void removeKeywordAt(int index);
    void scheduleMetadataCommit();
    void emitMetadataEdited();

    std::vector<Row> rows_;
    std::vector<std::vector<HSLRow>> hslRows_; // [colorIndex] -> rows
    std::vector<SplitToneRow> splitToneRows_;
    std::vector<MaskRow> maskRows_;
    lrc::Adjustments current_;
    QTimer debounce_;
    QTimer coarseDebounce_;
    // M11: leading-edge one-shot timer that bounds adjustmentsCommitted during a
    // mask/brush stroke to roughly one emit per kMaskGeometryCommitMs (instead
    // of once per mouse move).
    QTimer maskGeometryCommitTimer_;
    bool maskGeometryDirty_ = false;
    bool dragging_ = false;
    bool suppressSignals_ = false;
    bool vizActive_ = false;

    // --- masked local adjustments (UI) ------------------------------------
    QPushButton* maskLinearBtn_ = nullptr;
    QPushButton* maskRadialBtn_ = nullptr;
    QPushButton* maskBrushBtn_ = nullptr;
    QComboBox* maskCombo_ = nullptr;
    QPushButton* maskDeleteBtn_ = nullptr;
    QWidget* maskFeatherRow_ = nullptr;
    QSlider* maskFeatherSlider_ = nullptr;
    QDoubleSpinBox* maskFeatherSpin_ = nullptr;
    QWidget* maskBrushRow_ = nullptr;
    QSlider* maskBrushSizeSlider_ = nullptr;
    QWidget* maskSoftnessRow_ = nullptr;
    QSlider* maskSoftnessSlider_ = nullptr;
    QSlider* maskOpacitySlider_ = nullptr;
    QCheckBox* maskInvertCheck_ = nullptr;
    lrc::MaskType activeMaskTool_ = lrc::MaskType::None;

    // --- colour-range mask (UI) ----------------------------------------------
    QPushButton* maskColorBtn_ = nullptr;
    QWidget* maskColorRow_ = nullptr;
    QSlider* colorHueSlider_ = nullptr;
    QDoubleSpinBox* colorHueSpin_ = nullptr;
    QSlider* colorSatSlider_ = nullptr;
    QDoubleSpinBox* colorSatSpin_ = nullptr;
    QSlider* colorLumaSlider_ = nullptr;
    QDoubleSpinBox* colorLumaSpin_ = nullptr;
    QSlider* colorSoftnessSlider_ = nullptr;
    QDoubleSpinBox* colorSoftnessSpin_ = nullptr;
    QLabel* colorRefSwatch_ = nullptr;
    QPushButton* colorSampleBtn_ = nullptr;

    // --- range sub-mask (colour / luminance) UI (PHASE_4) --------------------
    QWidget* subRangeRow_ = nullptr;
    QCheckBox* subColorEnableCheck_ = nullptr;
    QLabel* subColorSwatch_ = nullptr;
    QPushButton* subColorSampleBtn_ = nullptr;
    QSlider* subColorRangeSlider_ = nullptr;
    QDoubleSpinBox* subColorRangeSpin_ = nullptr;
    QSlider* subColorSmoothSlider_ = nullptr;
    QDoubleSpinBox* subColorSmoothSpin_ = nullptr;
    QCheckBox* subLumaEnableCheck_ = nullptr;
    QLabel* subLumaValueLabel_ = nullptr;
    QPushButton* subLumaSampleBtn_ = nullptr;
    QSlider* subLumaRangeSlider_ = nullptr;
    QDoubleSpinBox* subLumaRangeSpin_ = nullptr;
    QSlider* subLumaSmoothSlider_ = nullptr;
    QDoubleSpinBox* subLumaSmoothSpin_ = nullptr;
    QComboBox* subUnionCombo_ = nullptr;

    // --- post-crop vignette (effects panel) --------------------------------
    QComboBox* vignetteStyleCombo_ = nullptr;

    // --- point-colour (ColorTarget) UI --------------------------------------
    QComboBox* colorTargetCombo_ = nullptr;
    QPushButton* colorTargetAddBtn_ = nullptr;
    QPushButton* colorTargetDeleteBtn_ = nullptr;
    QCheckBox* colorTargetEnabledCheck_ = nullptr;
    QPushButton* colorTargetSamplerBtn_ = nullptr;
    QLabel* colorTargetSwatch_ = nullptr;
    std::vector<ColorTargetRow> colorTargetRows_;

    // --- spot removal (M5) UI -----------------------------------------------
    QPushButton* spotHealBtn_ = nullptr;
    QPushButton* spotCloneBtn_ = nullptr;
    QSlider* spotRadiusSlider_ = nullptr;
    QDoubleSpinBox* spotRadiusSpin_ = nullptr;
    QLabel* spotCountLabel_ = nullptr;
    QPushButton* spotVisualizeBtn_ = nullptr;
    lrc::SpotType spotMode_ = lrc::SpotType::Heal;

    // --- metadata (M8) UI ---------------------------------------------------
    QLineEdit* metaTitleEdit_ = nullptr;
    QPlainTextEdit* metaCaptionEdit_ = nullptr;
    QLineEdit* metaCopyrightEdit_ = nullptr;
    QLineEdit* metaCreatorEdit_ = nullptr;
    QLineEdit* keywordInput_ = nullptr;
    QWidget* keywordChips_ = nullptr;
    QVBoxLayout* keywordChipsLayout_ = nullptr;
    std::vector<std::string> keywordNames_;
    QTimer metadataCommit_;

    HistogramWidget* histogram_ = nullptr;
    CurveEditor* curveEditor_ = nullptr;
    QTabWidget* hslTabs_ = nullptr;
    QPushButton* samplerButton_ = nullptr;
    ColorWheelWidget* highlightWheel_ = nullptr;
    ColorWheelWidget* shadowWheel_ = nullptr;
    QTimer wheelCommit_;
};
