#include "ui/DevelopPanel.h"

#include <algorithm>
#include <cmath>

#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QVBoxLayout>

#include "app/Constants.h"
#include "catalog/CatalogDatabase.h"
#include "ui/ColorWheelWidget.h"
#include "ui/CurveEditor.h"
#include "ui/HistogramWidget.h"
#include "ui/PanelSection.h"

namespace {

const char* kColorNames[8] = {
    QT_TR_NOOP("红"), QT_TR_NOOP("橙"), QT_TR_NOOP("黄"), QT_TR_NOOP("绿"),
    QT_TR_NOOP("青"), QT_TR_NOOP("蓝"), QT_TR_NOOP("紫"), QT_TR_NOOP("品红")};

// Creates a labelled slider+spin row and returns the container widget.
QWidget* makeSliderRow(QWidget* parent, QSlider*& slider,
                       QDoubleSpinBox*& spin, const char* label,
                       double min, double max, int decimals) {
    auto* h = new QHBoxLayout;
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);
    auto* lbl = new QLabel(parent->tr(label), parent);
    lbl->setFixedWidth(46);
    slider = new QSlider(Qt::Horizontal, parent);
    spin = new QDoubleSpinBox(parent);
    spin->setDecimals(decimals);
    spin->setSingleStep(std::pow(10.0, -static_cast<double>(decimals)));
    spin->setRange(min, max);
    slider->setRange(static_cast<int>(min * 100), static_cast<int>(max * 100));
    slider->setSingleStep(std::max(1, static_cast<int>(std::pow(10.0, 2.0 - decimals))));
    h->addWidget(lbl);
    h->addWidget(slider, 1);
    h->addWidget(spin);
    auto* container = new QWidget(parent);
    container->setLayout(h);
    return container;
}

// Converts a linear-RGB colour (as stored in lrc::ColorTarget) to a displayable
// sRGB QColor by applying the sRGB gamma curve (an approximation of the encoding
// ColorManager/lcms applies at the end of the pipeline).
QColor linearToSwatch(float r, float g, float b) {
    auto encode = [](float c) {
        c = std::clamp(c, 0.0f, 1.0f);
        c = (c <= 0.0031308f) ? (12.92f * c)
                              : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
        return std::clamp(c, 0.0f, 1.0f);
    };
    return QColor::fromRgbF(encode(r), encode(g), encode(b));
}

// Spot radius slider maps a 0..10000 int linearly onto the normalized radius
// range [kSpotRadiusMin, kSpotRadiusMax] (fraction of the longer image edge).
static constexpr double kSpotRadiusMin = 0.001;
static constexpr double kSpotRadiusMax = 0.2;

double spotRadiusFromSlider(int value) {
    return kSpotRadiusMin + (kSpotRadiusMax - kSpotRadiusMin) *
                                static_cast<double>(value) / 10000.0;
}

int spotSliderFromRadius(double radius) {
    radius = std::clamp(radius, kSpotRadiusMin, kSpotRadiusMax);
    return static_cast<int>(std::lround(
        (radius - kSpotRadiusMin) / (kSpotRadiusMax - kSpotRadiusMin) * 10000.0));
}

} // namespace

DevelopPanel::DevelopPanel(QWidget* parent)
    : QWidget(parent), debounce_(this), coarseDebounce_(this) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(2);

    // Histogram lives on top of the develop panel, LR-style.
    histogram_ = new HistogramWidget(this);
    histogram_->setFixedHeight(100);
    root->addWidget(histogram_);

    auto addSection = [&](const QString& title) {
        auto* section = new PanelSection(title, this);
        root->addWidget(section);
        return section;
    };

    // ---- 基本 ---------------------------------------------------------------
    PanelSection* basic = addSection(tr("基本"));

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(6);
    auto* autoBtn = new QPushButton(tr("自动"), this);
    autoBtn->setProperty("secondary", true);
    auto* resetBtn = new QPushButton(tr("复位"), this);
    resetBtn->setProperty("secondary", true);
    buttons->addWidget(autoBtn);
    buttons->addWidget(resetBtn);
    basic->addLayout(buttons);
    connect(autoBtn, &QPushButton::clicked, this, &DevelopPanel::autoRequested);
    connect(resetBtn, &QPushButton::clicked, this, &DevelopPanel::resetAll);

    auto makeBasicRow = [&](const char* label, double min, double max,
                            int decimals, double lrc::Adjustments::*field,
                            uint8_t vizMode = VizNone) {
        Row row;
        row.label = label;
        row.min = min;
        row.max = max;
        row.decimals = decimals;
        row.field = field;
        row.vizMode = vizMode;
        basic->addWidget(
            makeSliderRow(this, row.slider, row.spin, label, min, max, decimals));
        addRow(row);
    };
    makeBasicRow("色温", -100.0, 100.0, 0, &lrc::Adjustments::temperature);
    makeBasicRow("色调", -100.0, 100.0, 0, &lrc::Adjustments::tint);
    makeBasicRow("曝光", -5.0, 5.0, 2, &lrc::Adjustments::exposure, VizClipping);
    makeBasicRow("对比度", -100.0, 100.0, 0, &lrc::Adjustments::contrast);
    makeBasicRow("高光", -100.0, 100.0, 0, &lrc::Adjustments::highlights);
    makeBasicRow("阴影", -100.0, 100.0, 0, &lrc::Adjustments::shadows);
    makeBasicRow("白色", -100.0, 100.0, 0, &lrc::Adjustments::whites, VizClipping);
    makeBasicRow("黑色", -100.0, 100.0, 0, &lrc::Adjustments::blacks, VizClipping);
    makeBasicRow("纹理", -100.0, 100.0, 0, &lrc::Adjustments::texture, VizDetail);
    makeBasicRow("清晰度", -100.0, 100.0, 0, &lrc::Adjustments::clarity, VizDetail);
    makeBasicRow("去朦胧", -100.0, 100.0, 0, &lrc::Adjustments::dehaze, VizDetail);
    makeBasicRow("鲜艳度", -100.0, 100.0, 0, &lrc::Adjustments::vibrance);
    makeBasicRow("饱和度", -100.0, 100.0, 0, &lrc::Adjustments::saturation);

    // ---- 色调曲线 -----------------------------------------------------------
    PanelSection* curve = addSection(tr("色调曲线"));
    curveEditor_ = new CurveEditor(this);
    curveEditor_->setFixedHeight(100);
    curve->addWidget(curveEditor_);
    auto* curveReset = new QPushButton(tr("曲线复位"), this);
    curveReset->setProperty("secondary", true);
    curve->addWidget(curveReset);
    connect(curveReset, &QPushButton::clicked, this, [this]() {
        current_.curve.clear();
        curveEditor_->resetToLinear();
        scheduleDebounce();
        emit adjustmentsCommitted(current_);
    });
    connect(curveEditor_, &CurveEditor::curveChanged, this,
            [this](const std::vector<lrc::ToneCurvePoint>& points) {
                if (suppressSignals_)
                    return;
                current_.curve = points;
                scheduleDebounce();
            });
    // Curve drags take the coarse (30 ms) throttle while the handle is down,
    // then fall back to the refine pass on release. The single commit stays on
    // editingFinished (fired in the same release event) so one drag = one undo.
    connect(curveEditor_, &CurveEditor::dragStarted, this, [this]() {
        dragging_ = true;
    });
    connect(curveEditor_, &CurveEditor::dragEnded, this, [this]() {
        dragging_ = false;
        scheduleDebounce();
    });
    connect(curveEditor_, &CurveEditor::editingFinished, this, [this]() {
        if (!suppressSignals_)
            emit adjustmentsCommitted(current_);
    });

    // ---- 细节 ---------------------------------------------------------------
    PanelSection* detail = addSection(tr("细节"));
    auto makeDetailRow = [&](const char* label, double min, double max,
                             int decimals, double lrc::Adjustments::*field,
                             uint8_t vizMode = VizNone) {
        Row row;
        row.label = label;
        row.min = min;
        row.max = max;
        row.decimals = decimals;
        row.field = field;
        row.vizMode = vizMode;
        detail->addWidget(
            makeSliderRow(this, row.slider, row.spin, label, min, max, decimals));
        addRow(row);
    };
    makeDetailRow("锐化", 0.0, 150.0, 0, &lrc::Adjustments::sharpness);
    makeDetailRow("半径", 0.5, 3.0, 2, &lrc::Adjustments::sharpnessRadius);
    makeDetailRow("细节", 0.0, 100.0, 0, &lrc::Adjustments::sharpnessDetail);
    makeDetailRow("蒙版", 0.0, 100.0, 0, &lrc::Adjustments::sharpnessMasking, VizEdge);

    // Noise-reduction section (降噪), parallel to the sharpening block above.
    auto* nrLabel = new QLabel(tr("降噪"), this);
    nrLabel->setStyleSheet("color: #c8c8c8; font-weight: 500;");
    detail->addWidget(nrLabel);
    makeDetailRow("明亮度", 0.0, 100.0, 0, &lrc::Adjustments::luminance);
    makeDetailRow("细节",   0.0, 100.0, 0, &lrc::Adjustments::luminanceDetail);
    makeDetailRow("对比度", 0.0, 100.0, 0, &lrc::Adjustments::luminanceContrast);
    makeDetailRow("颜色",   0.0, 100.0, 0, &lrc::Adjustments::colorNoiseReduction);
    makeDetailRow("平滑",   0.0, 100.0, 0, &lrc::Adjustments::colorNoiseReductionSmoothness);

    // PHASE_12: AI Denoise (NAFNet) row. enabled+strength map to
    // Adjustments::aiDenoise; Pipeline::applySpatialAdjustments runs the
    // bundled NAFNet model when Pipeline::globalAiEngine() reports it ready.
    // When the model is missing, enabled has no effect (manual NR sliders
    // above remain authoritative) — so the control is always visible.
    auto* aiNrLabel = new QLabel(tr("AI 降噪"), this);
    aiNrLabel->setStyleSheet("color: #c8c8c8; font-weight: 500;");
    detail->addWidget(aiNrLabel);
    auto* aiNrRow = new QHBoxLayout;
    aiNrRow->setSpacing(6);
    aiDenoiseEnabledCheck_ = new QCheckBox(tr("启用"), this);
    aiDenoiseStrengthSlider_ = new QSlider(Qt::Horizontal, this);
    aiDenoiseStrengthSlider_->setRange(0, 100);
    aiDenoiseStrengthSlider_->setValue(0);
    aiDenoiseStrengthSpin_ = new QDoubleSpinBox(this);
    aiDenoiseStrengthSpin_->setRange(0.0, 100.0);
    aiDenoiseStrengthSpin_->setDecimals(0);
    aiDenoiseStrengthSpin_->setSingleStep(1.0);
    aiDenoiseStrengthSpin_->setSuffix("%");
    aiNrRow->addWidget(aiDenoiseEnabledCheck_);
    aiNrRow->addWidget(aiDenoiseStrengthSlider_, 1);
    aiNrRow->addWidget(aiDenoiseStrengthSpin_);
    detail->addLayout(aiNrRow);
    // Two-way binding: slider↔spin, enabled+strength→Adjustments::aiDenoise.
    connect(aiDenoiseStrengthSlider_, &QSlider::valueChanged, this,
            [this](int v) {
                if (suppressSignals_) return;
                QSignalBlocker b(aiDenoiseStrengthSpin_);
                aiDenoiseStrengthSpin_->setValue(static_cast<double>(v));
                current_.aiDenoise.strength = static_cast<float>(v);
                if (v > 0 && !current_.aiDenoise.enabled) {
                    current_.aiDenoise.enabled = true;
                    QSignalBlocker b2(aiDenoiseEnabledCheck_);
                    aiDenoiseEnabledCheck_->setChecked(true);
                }
                scheduleDebounce();
            });
    connect(aiDenoiseStrengthSpin_,
            qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double v) {
                if (suppressSignals_) return;
                QSignalBlocker b(aiDenoiseStrengthSlider_);
                aiDenoiseStrengthSlider_->setValue(static_cast<int>(v));
                current_.aiDenoise.strength = static_cast<float>(v);
                scheduleDebounce();
            });
    connect(aiDenoiseEnabledCheck_, &QCheckBox::toggled, this,
            [this](bool checked) {
                if (suppressSignals_) return;
                current_.aiDenoise.enabled = checked;
                // strength=0 + enabled reads as "no effect"; bump to a
                // sensible default on first enable so the user sees motion.
                if (checked && current_.aiDenoise.strength <= 0.0f) {
                    current_.aiDenoise.strength = 50.0f;
                    QSignalBlocker b1(aiDenoiseStrengthSlider_);
                    QSignalBlocker b2(aiDenoiseStrengthSpin_);
                    aiDenoiseStrengthSlider_->setValue(50);
                    aiDenoiseStrengthSpin_->setValue(50.0);
                }
                scheduleDebounce();
            });

    // ---- 颜色 (HSL 各通道) ---------------------------------------------------
    PanelSection* color = addSection(tr("颜色"));

    hslTabs_ = new QTabWidget(this);
    hslTabs_->setTabPosition(QTabWidget::North);
    hslTabs_->setDocumentMode(true);

    struct HSLTabInfo {
        const char* title;
        double lrc::HSLChannel::*field;
        double min, max;
    };
    const HSLTabInfo tabInfos[3] = {
        {QT_TR_NOOP("色相"), &lrc::HSLChannel::hue, -180.0, 180.0},
        {QT_TR_NOOP("饱和度"), &lrc::HSLChannel::saturation, -100.0, 100.0},
        {QT_TR_NOOP("明亮"), &lrc::HSLChannel::luminance, -100.0, 100.0},
    };

    hslRows_.resize(8);
    for (int tabIdx = 0; tabIdx < 3; ++tabIdx) {
        auto* tabWidget = new QWidget(this);
        auto* tabLayout = new QVBoxLayout(tabWidget);
        tabLayout->setContentsMargins(0, 2, 0, 0);
        tabLayout->setSpacing(1);

        for (int ci = 0; ci < 8; ++ci) {
            HSLRow r;
            r.label = kColorNames[ci];
            r.field = tabInfos[tabIdx].field;
            r.min = tabInfos[tabIdx].min;
            r.max = tabInfos[tabIdx].max;
            // Create the widgets first so addHSLRow connects to real objects
            // (it syncs the slider/spin immediately, which requires non-null).
            tabLayout->addWidget(makeSliderRow(
                tabWidget, r.slider, r.spin, r.label, r.min, r.max, 1));
            addHSLRow(ci, r);
        }
        tabLayout->addStretch(1);
        hslTabs_->addTab(tabWidget, tr(tabInfos[tabIdx].title));
    }
    color->addWidget(hslTabs_);

    samplerButton_ = new QPushButton(tr("点选吸管"), this);
    samplerButton_->setProperty("secondary", true);
    samplerButton_->setCheckable(true);
    color->addWidget(samplerButton_);
    connect(samplerButton_, &QPushButton::toggled, this,
            [this](bool active) { emit colorSamplerRequested(active); });

    // ---- 色调分离 -----------------------------------------------------------
    PanelSection* st = addSection(tr("色调分离"));
    auto addSplitSliderRow = [&](const char* label,
                                 double lrc::SplitTone::*field, double min,
                                 double max, int decimals) {
        SplitToneRow row;
        row.label = label;
        row.field = field;
        row.min = min;
        row.max = max;
        st->addWidget(makeSliderRow(this, row.slider, row.spin, label, min, max, decimals));
        addSplitToneRow(row);
    };
    auto* hlLabel = new QLabel(tr("高光"), this);
    hlLabel->setStyleSheet("color: #c8c8c8; font-weight: 500;");
    st->addWidget(hlLabel);
    addSplitSliderRow("色相", &lrc::SplitTone::highlightHue, 0.0, 360.0, 1);
    addSplitSliderRow("饱和度", &lrc::SplitTone::highlightSaturation, 0.0, 100.0, 0);
    auto* shLabel = new QLabel(tr("阴影"), this);
    shLabel->setStyleSheet("color: #888888; font-weight: 500;");
    st->addWidget(shLabel);
    addSplitSliderRow("色相", &lrc::SplitTone::shadowHue, 0.0, 360.0, 1);
    addSplitSliderRow("饱和度", &lrc::SplitTone::shadowSaturation, 0.0, 100.0, 0);
    addSplitSliderRow("平衡", &lrc::SplitTone::balance, -100.0, 100.0, 0);

    // Visual colour wheels (可视化颜色调整): angle = hue, radius = saturation.
    auto* wheelsRow = new QHBoxLayout;
    wheelsRow->setSpacing(8);
    highlightWheel_ = new ColorWheelWidget(this);
    shadowWheel_ = new ColorWheelWidget(this);
    wheelsRow->addWidget(highlightWheel_, 1);
    wheelsRow->addWidget(shadowWheel_, 1);
    st->addLayout(wheelsRow);

    auto commitFromWheel = [this]() { emit adjustmentsCommitted(current_); };
    wheelCommit_.setSingleShot(true);
    wheelCommit_.setInterval(lrc::app::throttle::kColorWheelCommitMs);
    connect(&wheelCommit_, &QTimer::timeout, this, commitFromWheel);

    auto wheelChanged = [this](bool highlight) {
        return [this, highlight](double h, double s) {
            if (suppressSignals_)
                return;
            lrc::SplitTone& st = current_.splitTone;
            if (highlight) {
                st.highlightHue = h;
                st.highlightSaturation = s;
            } else {
                st.shadowHue = h;
                st.shadowSaturation = s;
            }
            // 同步对应的色相/饱和度滑块，避免后续拖动把色轮设定值覆盖掉 (keep wheel in sync)
            for (const SplitToneRow& row : splitToneRows_) {
                const bool match = highlight
                    ? (row.field == &lrc::SplitTone::highlightHue ||
                       row.field == &lrc::SplitTone::highlightSaturation)
                    : (row.field == &lrc::SplitTone::shadowHue ||
                       row.field == &lrc::SplitTone::shadowSaturation);
                if (match)
                    syncSplitToneRow(row);
            }
            scheduleDebounce();
            wheelCommit_.start();
        };
    };
    connect(highlightWheel_, &ColorWheelWidget::colorChanged,
            this, wheelChanged(true));
    connect(shadowWheel_, &ColorWheelWidget::colorChanged,
            this, wheelChanged(false));

    // ---- 点颜色 -------------------------------------------------------------
    PanelSection* pc = addSection(tr("点颜色"));

    // Target list + add / delete.
    auto* colorTargetComboRow = new QHBoxLayout;
    colorTargetComboRow->setSpacing(6);
    colorTargetCombo_ = new QComboBox(this);
    colorTargetAddBtn_ = new QPushButton(tr("添加"), this);
    colorTargetAddBtn_->setProperty("secondary", true);
    colorTargetDeleteBtn_ = new QPushButton(tr("删除"), this);
    colorTargetDeleteBtn_->setProperty("secondary", true);
    colorTargetDeleteBtn_->setEnabled(false);
    colorTargetComboRow->addWidget(colorTargetCombo_, 1);
    colorTargetComboRow->addWidget(colorTargetAddBtn_);
    colorTargetComboRow->addWidget(colorTargetDeleteBtn_);
    pc->addLayout(colorTargetComboRow);
    connect(colorTargetCombo_,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &DevelopPanel::onColorTargetSelected);
    connect(colorTargetAddBtn_, &QPushButton::clicked, this,
            &DevelopPanel::onColorTargetAdd);
    connect(colorTargetDeleteBtn_, &QPushButton::clicked, this,
            &DevelopPanel::onColorTargetDelete);

    // Enabled + reference sampler + swatch.
    auto* colorTargetToolRow = new QHBoxLayout;
    colorTargetToolRow->setSpacing(6);
    colorTargetEnabledCheck_ = new QCheckBox(tr("启用"), this);
    colorTargetSamplerBtn_ = new QPushButton(tr("取参考色"), this);
    colorTargetSamplerBtn_->setCheckable(true);
    colorTargetSamplerBtn_->setProperty("secondary", true);
    colorTargetSwatch_ = new QLabel(this);
    colorTargetSwatch_->setFixedSize(34, 18);
    colorTargetSwatch_->setStyleSheet(
        "background-color: #2a2a2a; border: 1px solid #666666; border-radius: 3px;");
    colorTargetToolRow->addWidget(colorTargetEnabledCheck_);
    colorTargetToolRow->addWidget(colorTargetSamplerBtn_);
    colorTargetToolRow->addWidget(colorTargetSwatch_, 1);
    pc->addLayout(colorTargetToolRow);
    connect(colorTargetEnabledCheck_, &QCheckBox::toggled, this,
            &DevelopPanel::onColorTargetEnabledToggled);
    connect(colorTargetSamplerBtn_, &QPushButton::toggled, this,
            [this](bool active) { emit colorTargetSamplerRequested(active); });

    // Adjustment rows (mirror the MaskRow pattern; one row per field).
    auto addColorTargetSliderRow = [&](const char* label, double min, double max,
                                       int decimals,
                                       double lrc::ColorTarget::*field) {
        ColorTargetRow row;
        row.label = label;
        row.field = field;
        row.min = min;
        row.max = max;
        pc->addWidget(makeSliderRow(this, row.slider, row.spin, label, min, max,
                                    decimals));
        addColorTargetRow(row);
    };
    addColorTargetSliderRow("色相", -180.0, 180.0, 1, &lrc::ColorTarget::hueShift);
    addColorTargetSliderRow("饱和度", -100.0, 100.0, 0, &lrc::ColorTarget::saturation);
    addColorTargetSliderRow("明亮", -100.0, 100.0, 0, &lrc::ColorTarget::luminance);
    addColorTargetSliderRow("范围", 0.0, 100.0, 0, &lrc::ColorTarget::range);
    addColorTargetSliderRow("平滑", 0.0, 100.0, 0, &lrc::ColorTarget::smoothness);
    addColorTargetSliderRow("强度", 0.0, 100.0, 0, &lrc::ColorTarget::amount);

    // ---- 效果 ---------------------------------------------------------------
    PanelSection* effects = addSection(tr("效果"));
    auto makeEffectsRow = [&](const char* label, double min, double max,
                              int decimals, double lrc::Adjustments::*field) {
        Row row;
        row.label = label;
        row.min = min;
        row.max = max;
        row.decimals = decimals;
        row.field = field;
        effects->addWidget(
            makeSliderRow(this, row.slider, row.spin, label, min, max, decimals));
        addRow(row);
    };
    makeEffectsRow("颗粒", 0.0, 100.0, 0, &lrc::Adjustments::grain);
    makeEffectsRow("颗粒大小", 0.0, 100.0, 0, &lrc::Adjustments::grainSize);
    makeEffectsRow("粗糙度", 0.0, 100.0, 0, &lrc::Adjustments::grainRoughness);
    // Post-crop vignette: the "晕影" row is the Amount (-100..+100); the shape
    // rows below follow LR's post-crop vignette panel. (PHASE_3)
    makeEffectsRow("晕影", -100.0, 100.0, 0, &lrc::Adjustments::vignette);
    makeEffectsRow("中点", 0.0, 100.0, 0, &lrc::Adjustments::vignetteMidpoint);
    makeEffectsRow("圆度", -100.0, 100.0, 0, &lrc::Adjustments::vignetteRoundness);
    makeEffectsRow("羽化", 0.0, 100.0, 0, &lrc::Adjustments::vignetteFeather);
    makeEffectsRow("高光", 0.0, 100.0, 0, &lrc::Adjustments::vignetteHighlights);

    // Style dropdown: only Highlight Priority is implemented (PHASE_3); Paint
    // Overlay / Color Priority are shown but disabled with a TODO hint.
    auto* styleRow = new QHBoxLayout;
    styleRow->setContentsMargins(0, 0, 0, 0);
    styleRow->setSpacing(6);
    auto* styleLabel = new QLabel(tr("样式"), this);
    styleLabel->setFixedWidth(46);
    vignetteStyleCombo_ = new QComboBox(this);
    vignetteStyleCombo_->addItem(tr("Paint Overlay (TODO)"));
    vignetteStyleCombo_->addItem(tr("Color Priority (TODO)"));
    vignetteStyleCombo_->addItem(tr("Highlight Priority"));
    vignetteStyleCombo_->setCurrentIndex(2);
    if (auto* model = qobject_cast<QStandardItemModel*>(
            vignetteStyleCombo_->model())) {
        for (int i = 0; i < 2; ++i)
            if (auto* item = model->item(i))
                item->setEnabled(false);
    }
    styleRow->addWidget(styleLabel);
    styleRow->addWidget(vignetteStyleCombo_, 1);
    effects->addLayout(styleRow);
    connect(vignetteStyleCombo_,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (suppressSignals_)
                    return;
                current_.vignetteStyle = std::clamp(index, 0, 2);
                scheduleDebounce();
                if (!dragging_)
                    emit adjustmentsCommitted(current_);
            });

    // ---- 镜头校正 -----------------------------------------------------------
    PanelSection* lens = addSection(tr("镜头校正"));
    auto makeLensRow = [&](const char* label, double min, double max,
                           int decimals, double lrc::Adjustments::*field) {
        Row row;
        row.label = label;
        row.min = min;
        row.max = max;
        row.decimals = decimals;
        row.field = field;
        lens->addWidget(
            makeSliderRow(this, row.slider, row.spin, label, min, max, decimals));
        addRow(row);
    };
    makeLensRow("几何畸变", -100.0, 100.0, 0, &lrc::Adjustments::distortion);

    // ---- 蒙版（局部调整） ---------------------------------------------------
    PanelSection* maskSec = addSection(tr("蒙版"));

    // Tool buttons: linear gradient / radial gradient / brush, exclusive.
    auto* maskToolsRow = new QHBoxLayout;
    maskToolsRow->setSpacing(6);
    maskLinearBtn_ = new QPushButton(tr("线性渐变"), this);
    maskRadialBtn_ = new QPushButton(tr("径向渐变"), this);
    maskBrushBtn_ = new QPushButton(tr("画笔"), this);
    maskColorBtn_ = new QPushButton(tr("颜色"), this);
    for (QPushButton* b : {maskLinearBtn_, maskRadialBtn_, maskBrushBtn_,
                           maskColorBtn_}) {
        b->setCheckable(true);
        b->setProperty("secondary", true);
        maskToolsRow->addWidget(b);
    }
    maskSec->addLayout(maskToolsRow);
    auto* maskToolGroup = new QButtonGroup(this);
    maskToolGroup->setExclusive(true);
    maskToolGroup->addButton(maskLinearBtn_);
    maskToolGroup->addButton(maskRadialBtn_);
    maskToolGroup->addButton(maskBrushBtn_);
    maskToolGroup->addButton(maskColorBtn_);
    connect(maskLinearBtn_, &QPushButton::toggled, this,
            [this](bool c) { onMaskToolToggled(lrc::MaskType::LinearGradient, c); });
    connect(maskRadialBtn_, &QPushButton::toggled, this,
            [this](bool c) { onMaskToolToggled(lrc::MaskType::RadialGradient, c); });
    connect(maskBrushBtn_, &QPushButton::toggled, this,
            [this](bool c) { onMaskToolToggled(lrc::MaskType::Brush, c); });
    connect(maskColorBtn_, &QPushButton::toggled, this,
            [this](bool c) { onMaskToolToggled(lrc::MaskType::ColorRange, c); });

    // PHASE_13: AI segmentation mask buttons (subject / sky / background).
    // Unlike the geometry tools above, these are one-shot click buttons:
    // clicking creates a new Mask with type=AiMask and the chosen target,
    // immediately rasterized by Pipeline::applyLocalAdjustments via the
    // bundled BiRefNet/u²Net model. No interactive drawing step is needed.
    auto* aiMaskRow = new QHBoxLayout;
    aiMaskRow->setSpacing(6);
    maskAiSubjectBtn_ = new QPushButton(tr("AI 主体"), this);
    maskAiSkyBtn_ = new QPushButton(tr("AI 天空"), this);
    maskAiBackgroundBtn_ = new QPushButton(tr("AI 背景"), this);
    for (QPushButton* b : {maskAiSubjectBtn_, maskAiSkyBtn_,
                           maskAiBackgroundBtn_}) {
        b->setProperty("secondary", true);
        aiMaskRow->addWidget(b);
    }
    maskSec->addLayout(aiMaskRow);
    connect(maskAiSubjectBtn_, &QPushButton::clicked, this,
            [this]() { addAiMask(lrc::AiMaskTarget::Subject); });
    connect(maskAiSkyBtn_, &QPushButton::clicked, this,
            [this]() { addAiMask(lrc::AiMaskTarget::Sky); });
    connect(maskAiBackgroundBtn_, &QPushButton::clicked, this,
            [this]() { addAiMask(lrc::AiMaskTarget::Background); });

    // Mask list + delete.
    auto* maskComboRow = new QHBoxLayout;
    maskComboRow->setSpacing(6);
    maskCombo_ = new QComboBox(this);
    maskDeleteBtn_ = new QPushButton(tr("删除"), this);
    maskDeleteBtn_->setProperty("secondary", true);
    maskDeleteBtn_->setEnabled(false);
    maskComboRow->addWidget(maskCombo_, 1);
    maskComboRow->addWidget(maskDeleteBtn_);
    maskSec->addLayout(maskComboRow);
    connect(maskCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DevelopPanel::onMaskSelected);
    connect(maskDeleteBtn_, &QPushButton::clicked, this, &DevelopPanel::onMaskDelete);

    // Feather row (shown for linear/radial masks): 0..100 -> feather 0..1.
    maskFeatherRow_ = makeSliderRow(this, maskFeatherSlider_, maskFeatherSpin_,
                                    "羽化", 0.0, 100.0, 0);
    maskSec->addWidget(maskFeatherRow_);
    maskFeatherRow_->setVisible(false);
    connect(maskFeatherSlider_, &QSlider::valueChanged, this,
            [this](int v) {
                onMaskFeatherChanged(v);
                // Keyboard edits never release the slider, so commit here too.
                if (!suppressSignals_ && !dragging_)
                    emit adjustmentsCommitted(current_);
            });
    connectSliderGesture(maskFeatherSlider_);
    connect(maskFeatherSlider_, &QSlider::sliderReleased, this, [this]() {
        if (!suppressSignals_)
            emit adjustmentsCommitted(current_);
    });
    connect(maskFeatherSpin_,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double v) {
                onMaskFeatherChanged(static_cast<int>(v * 100.0));
            });
    connect(maskFeatherSpin_, &QDoubleSpinBox::editingFinished, this, [this]() {
        if (!suppressSignals_)
            emit adjustmentsCommitted(current_);
    });

    // Brush size row (shown for brush masks): slider 0..10000 -> radius /20000
    // (0..0.5 normalized). A spin box would be overkill for a tool setting.
    auto makeSliderOnlyRow = [&](const char* label, QSlider*& slider) {
        auto* h = new QHBoxLayout;
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(6);
        auto* lbl = new QLabel(tr(label), this);
        lbl->setFixedWidth(46);
        slider = new QSlider(Qt::Horizontal, this);
        slider->setRange(0, 10000);
        slider->setSingleStep(100);
        h->addWidget(lbl);
        h->addWidget(slider, 1);
        auto* container = new QWidget(this);
        container->setLayout(h);
        return container;
    };
    maskBrushRow_ = makeSliderOnlyRow("大小", maskBrushSizeSlider_);
    maskSec->addWidget(maskBrushRow_);
    maskBrushRow_->setVisible(false);
    connect(maskBrushSizeSlider_, &QSlider::valueChanged, this,
            &DevelopPanel::onMaskBrushSizeChanged);
    connectSliderGesture(maskBrushSizeSlider_);
    maskBrushSizeSlider_->setValue(2500);  // 12.5% -> radius 0.125

    // Brush softness row (shown for brush masks): slider 0..10000 -> 0..1.
    // Default 5000 (0.5). Only affects new dabs, so it emits immediately.
    maskSoftnessRow_ = makeSliderOnlyRow("软度", maskSoftnessSlider_);
    maskSec->addWidget(maskSoftnessRow_);
    maskSoftnessRow_->setVisible(false);
    connect(maskSoftnessSlider_, &QSlider::valueChanged, this,
            &DevelopPanel::onMaskSoftnessChanged);
    connectSliderGesture(maskSoftnessSlider_);
    maskSoftnessSlider_->setValue(5000);  // 0.5

    // Colour-range mask tolerance rows (shown for colour-range masks): hue in
    // degrees [0,180] (default 40); sat/luma/softness as percentages [0,100]
    // stored as /100 (defaults 0.5). All inside maskColorRow_, hidden until a
    // colour-range mask is active.
    maskColorRow_ = new QWidget(this);
    auto* maskColorLayout = new QVBoxLayout(maskColorRow_);
    maskColorLayout->setContentsMargins(0, 0, 0, 0);
    maskColorLayout->setSpacing(2);

    QWidget* hueRow =
        makeSliderRow(this, colorHueSlider_, colorHueSpin_, "色相", 0.0, 180.0, 0);
    maskColorLayout->addWidget(hueRow);
    colorHueSlider_->setValue(4000);   // default hueTolerance 40 degrees.
    colorHueSpin_->setValue(40.0);

    QWidget* satRow =
        makeSliderRow(this, colorSatSlider_, colorSatSpin_, "饱和", 0.0, 100.0, 0);
    maskColorLayout->addWidget(satRow);
    colorSatSlider_->setValue(5000);   // default satTolerance 0.5.
    colorSatSpin_->setValue(50.0);

    QWidget* lumaRow =
        makeSliderRow(this, colorLumaSlider_, colorLumaSpin_, "明度", 0.0, 100.0, 0);
    maskColorLayout->addWidget(lumaRow);
    colorLumaSlider_->setValue(5000);  // default lumaTolerance 0.5.
    colorLumaSpin_->setValue(50.0);

    QWidget* softRow =
        makeSliderRow(this, colorSoftnessSlider_, colorSoftnessSpin_, "软度",
                      0.0, 100.0, 0);
    maskColorLayout->addWidget(softRow);
    colorSoftnessSlider_->setValue(5000);  // default softness 0.5.
    colorSoftnessSpin_->setValue(50.0);

    // Reference swatch (grey until sampled) + sampler button.
    auto* maskColorToolRow = new QHBoxLayout;
    maskColorToolRow->setSpacing(6);
    colorRefSwatch_ = new QLabel(this);
    colorRefSwatch_->setFixedSize(34, 18);
    colorSampleBtn_ = new QPushButton(tr("取样"), this);
    colorSampleBtn_->setCheckable(true);
    colorSampleBtn_->setProperty("secondary", true);
    maskColorToolRow->addWidget(colorRefSwatch_);
    maskColorToolRow->addWidget(colorSampleBtn_, 1);
    maskColorLayout->addLayout(maskColorToolRow);
    updateColorRangeSwatch(QColor());   // disabled grey until a reference exists.

    // Tolerance sliders start disabled; syncColorRangeSection enables them once
    // a reference colour is sampled.
    colorHueSlider_->setEnabled(false);
    colorHueSpin_->setEnabled(false);
    colorSatSlider_->setEnabled(false);
    colorSatSpin_->setEnabled(false);
    colorLumaSlider_->setEnabled(false);
    colorLumaSpin_->setEnabled(false);
    colorSoftnessSlider_->setEnabled(false);
    colorSoftnessSpin_->setEnabled(false);

    // Mirror the opacity-slider pattern: valueChanged updates the model and the
    // spin, sliderReleased (or keyboard) commits, spin editingFinished commits.
    auto connectColorTolerance = [&](QSlider*& slider, QDoubleSpinBox*& spin) {
        connect(slider, &QSlider::valueChanged, this,
                &DevelopPanel::onColorRangeToleranceSlider);
        connectSliderGesture(slider);
        connect(slider, &QSlider::sliderReleased, this, [this]() {
            if (!suppressSignals_)
                emit adjustmentsCommitted(current_);
        });
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &DevelopPanel::onColorRangeToleranceSpin);
        connect(spin, &QDoubleSpinBox::editingFinished, this, [this]() {
            if (!suppressSignals_)
                emit adjustmentsCommitted(current_);
        });
    };
    connectColorTolerance(colorHueSlider_, colorHueSpin_);
    connectColorTolerance(colorSatSlider_, colorSatSpin_);
    connectColorTolerance(colorLumaSlider_, colorLumaSpin_);
    connectColorTolerance(colorSoftnessSlider_, colorSoftnessSpin_);

    connect(colorSampleBtn_, &QPushButton::toggled, this,
            [this](bool active) { onColorRangeSampleToggled(active); });

    maskSec->addWidget(maskColorRow_);
    maskColorRow_->setVisible(false);

    // --- PHASE_4: range sub-mask rows (colour range + luminance range). These
    // layer pixel-content modifiers on top of a geometry mask, shown only for
    // linear/radial/brush masks (updateMaskSectionVisibility).
    subRangeRow_ = new QWidget(this);
    auto* subRangeLayout = new QVBoxLayout(subRangeRow_);
    subRangeLayout->setContentsMargins(0, 0, 0, 0);
    subRangeLayout->setSpacing(2);

    auto makeSubTitle = [this](const char* text) {
        auto* lbl = new QLabel(tr(text), this);
        lbl->setStyleSheet("color: #c8c8c8; font-weight: 500;");
        return lbl;
    };

    // Colour-range sub-mask.
    subRangeLayout->addWidget(makeSubTitle("颜色范围"));
    subColorEnableCheck_ = new QCheckBox(tr("启用"), this);
    subRangeLayout->addWidget(subColorEnableCheck_);
    auto* subColorToolRow = new QHBoxLayout;
    subColorToolRow->setSpacing(6);
    subColorSwatch_ = new QLabel(this);
    subColorSwatch_->setFixedSize(34, 18);
    subColorSampleBtn_ = new QPushButton(tr("取色"), this);
    subColorSampleBtn_->setCheckable(true);
    subColorSampleBtn_->setProperty("secondary", true);
    subColorToolRow->addWidget(subColorSwatch_);
    subColorToolRow->addWidget(subColorSampleBtn_, 1);
    subRangeLayout->addLayout(subColorToolRow);
    QWidget* subColorRangeRow = makeSliderRow(
        this, subColorRangeSlider_, subColorRangeSpin_, "范围", 0.0, 100.0, 0);
    subRangeLayout->addWidget(subColorRangeRow);
    QWidget* subColorSmoothRow = makeSliderRow(
        this, subColorSmoothSlider_, subColorSmoothSpin_, "平滑", 0.0, 100.0, 0);
    subRangeLayout->addWidget(subColorSmoothRow);

    // Luminance-range sub-mask.
    subRangeLayout->addWidget(makeSubTitle("明亮度范围"));
    subLumaEnableCheck_ = new QCheckBox(tr("启用"), this);
    subRangeLayout->addWidget(subLumaEnableCheck_);
    auto* subLumaToolRow = new QHBoxLayout;
    subLumaToolRow->setSpacing(6);
    subLumaValueLabel_ = new QLabel(tr("参考: --"), this);
    subLumaSampleBtn_ = new QPushButton(tr("取样"), this);
    subLumaSampleBtn_->setCheckable(true);
    subLumaSampleBtn_->setProperty("secondary", true);
    subLumaToolRow->addWidget(subLumaValueLabel_, 1);
    subLumaToolRow->addWidget(subLumaSampleBtn_);
    subRangeLayout->addLayout(subLumaToolRow);
    QWidget* subLumaRangeRow = makeSliderRow(
        this, subLumaRangeSlider_, subLumaRangeSpin_, "范围", 0.0, 100.0, 0);
    subRangeLayout->addWidget(subLumaRangeRow);
    QWidget* subLumaSmoothRow = makeSliderRow(
        this, subLumaSmoothSlider_, subLumaSmoothSpin_, "平滑", 0.0, 100.0, 0);
    subRangeLayout->addWidget(subLumaSmoothRow);

    // Combination mode: intersection (multiply) / union (max).
    auto* subUnionRow = new QHBoxLayout;
    subUnionRow->setSpacing(6);
    auto* subUnionLabel = new QLabel(tr("组合"), this);
    subUnionLabel->setFixedWidth(46);
    subUnionCombo_ = new QComboBox(this);
    subUnionCombo_->addItem(tr("交集"));
    subUnionCombo_->addItem(tr("并集"));
    subUnionRow->addWidget(subUnionLabel);
    subUnionRow->addWidget(subUnionCombo_, 1);
    subRangeLayout->addLayout(subUnionRow);

    maskSec->addWidget(subRangeRow_);
    subRangeRow_->setVisible(false);

    connect(subColorEnableCheck_, &QCheckBox::toggled, this,
            &DevelopPanel::onSubColorEnableToggled);
    connect(subColorSampleBtn_, &QPushButton::toggled, this,
            [this](bool active) { onSubColorSampleToggled(active); });
    connect(subColorRangeSlider_, &QSlider::valueChanged, this,
            &DevelopPanel::onSubColorRangeSlider);
    connectSliderGesture(subColorRangeSlider_);
    connect(subColorRangeSlider_, &QSlider::sliderReleased, this, [this]() {
        if (!suppressSignals_)
            emit adjustmentsCommitted(current_);
    });
    connect(subColorRangeSpin_,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &DevelopPanel::onSubColorRangeSpin);
    connect(subColorRangeSpin_, &QDoubleSpinBox::editingFinished, this,
            [this]() {
                if (!suppressSignals_)
                    emit adjustmentsCommitted(current_);
            });
    connect(subColorSmoothSlider_, &QSlider::valueChanged, this,
            &DevelopPanel::onSubColorSmoothSlider);
    connectSliderGesture(subColorSmoothSlider_);
    connect(subColorSmoothSlider_, &QSlider::sliderReleased, this, [this]() {
        if (!suppressSignals_)
            emit adjustmentsCommitted(current_);
    });
    connect(subColorSmoothSpin_,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &DevelopPanel::onSubColorSmoothSpin);
    connect(subColorSmoothSpin_, &QDoubleSpinBox::editingFinished, this,
            [this]() {
                if (!suppressSignals_)
                    emit adjustmentsCommitted(current_);
            });

    connect(subLumaEnableCheck_, &QCheckBox::toggled, this,
            &DevelopPanel::onSubLumaEnableToggled);
    connect(subLumaSampleBtn_, &QPushButton::toggled, this,
            [this](bool active) { onSubLumaSampleToggled(active); });
    connect(subLumaRangeSlider_, &QSlider::valueChanged, this,
            &DevelopPanel::onSubLumaRangeSlider);
    connectSliderGesture(subLumaRangeSlider_);
    connect(subLumaRangeSlider_, &QSlider::sliderReleased, this, [this]() {
        if (!suppressSignals_)
            emit adjustmentsCommitted(current_);
    });
    connect(subLumaRangeSpin_,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &DevelopPanel::onSubLumaRangeSpin);
    connect(subLumaRangeSpin_, &QDoubleSpinBox::editingFinished, this,
            [this]() {
                if (!suppressSignals_)
                    emit adjustmentsCommitted(current_);
            });
    connect(subLumaSmoothSlider_, &QSlider::valueChanged, this,
            &DevelopPanel::onSubLumaSmoothSlider);
    connectSliderGesture(subLumaSmoothSlider_);
    connect(subLumaSmoothSlider_, &QSlider::sliderReleased, this, [this]() {
        if (!suppressSignals_)
            emit adjustmentsCommitted(current_);
    });
    connect(subLumaSmoothSpin_,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &DevelopPanel::onSubLumaSmoothSpin);
    connect(subLumaSmoothSpin_, &QDoubleSpinBox::editingFinished, this,
            [this]() {
                if (!suppressSignals_)
                    emit adjustmentsCommitted(current_);
            });

    connect(subUnionCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DevelopPanel::onSubUnionChanged);

    updateSubColorSwatch(QColor());
    updateSubLumaValueLabel(-1.0);
    // Defaults: disabled until a geometry mask is selected.
    subColorRangeSlider_->setValue(5000);  // colorRange 50.
    subColorRangeSpin_->setValue(50.0);
    subColorSmoothSlider_->setValue(5000);  // colorSmoothness 50.
    subColorSmoothSpin_->setValue(50.0);
    subLumaRangeSlider_->setValue(5000);  // luminanceRange 50.
    subLumaRangeSpin_->setValue(50.0);
    subLumaSmoothSlider_->setValue(5000);  // luminanceSmoothness 50.
    subLumaSmoothSpin_->setValue(50.0);

    QWidget* opacityRow = makeSliderOnlyRow("不透明度", maskOpacitySlider_);
    maskSec->addWidget(opacityRow);
    connect(maskOpacitySlider_, &QSlider::valueChanged, this,
            [this](int v) {
                onMaskOpacityChanged(v);
                // Keyboard edits never release the slider, so commit here too.
                if (!suppressSignals_ && !dragging_)
                    emit adjustmentsCommitted(current_);
            });
    connectSliderGesture(maskOpacitySlider_);
    connect(maskOpacitySlider_, &QSlider::sliderReleased, this, [this]() {
        if (!suppressSignals_)
            emit adjustmentsCommitted(current_);
    });

    maskInvertCheck_ = new QCheckBox(tr("反转蒙版"), this);
    maskSec->addWidget(maskInvertCheck_);
    connect(maskInvertCheck_, &QCheckBox::toggled, this,
            &DevelopPanel::onMaskInvertToggled);

    // Local adjustments (MaskRow): mirror a subset of the global stack.
    auto addMaskSliderRow = [&](const char* label, double min, double max,
                                int decimals, double lrc::Mask::*field) {
        MaskRow row;
        row.label = label;
        row.min = min;
        row.max = max;
        row.field = field;
        maskSec->addWidget(
            makeSliderRow(this, row.slider, row.spin, label, min, max, decimals));
        addMaskRow(row);
    };
    addMaskSliderRow("曝光", -5.0, 5.0, 2, &lrc::Mask::exposure);
    addMaskSliderRow("对比度", -100.0, 100.0, 0, &lrc::Mask::contrast);
    addMaskSliderRow("高光", -100.0, 100.0, 0, &lrc::Mask::highlights);
    addMaskSliderRow("阴影", -100.0, 100.0, 0, &lrc::Mask::shadows);
    addMaskSliderRow("清晰度", -100.0, 100.0, 0, &lrc::Mask::clarity);
    addMaskSliderRow("饱和度", -100.0, 100.0, 0, &lrc::Mask::saturation);
    addMaskSliderRow("色温", -100.0, 100.0, 0, &lrc::Mask::temperature);
    addMaskSliderRow("色调", -100.0, 100.0, 0, &lrc::Mask::tint);
    addMaskSliderRow("白色", -100.0, 100.0, 0, &lrc::Mask::whites);
    addMaskSliderRow("黑色", -100.0, 100.0, 0, &lrc::Mask::blacks);
    addMaskSliderRow("去朦胧", -100.0, 100.0, 0, &lrc::Mask::dehaze);

    // ---- 污点去除 (spot removal / healing brush, M5) ------------------------
    PanelSection* spotSec = addSection(tr("污点去除"));

    // Heal / Clone mode buttons, exclusive (LR: 修复 / 仿制). Heal is default.
    auto* spotToolsRow = new QHBoxLayout;
    spotToolsRow->setSpacing(6);
    spotHealBtn_ = new QPushButton(tr("修复"), this);
    spotCloneBtn_ = new QPushButton(tr("仿制"), this);
    for (QPushButton* b : {spotHealBtn_, spotCloneBtn_}) {
        b->setCheckable(true);
        b->setProperty("secondary", true);
        spotToolsRow->addWidget(b);
    }
    spotSec->addLayout(spotToolsRow);
    auto* spotToolGroup = new QButtonGroup(this);
    spotToolGroup->setExclusive(true);
    spotToolGroup->addButton(spotHealBtn_);
    spotToolGroup->addButton(spotCloneBtn_);
    spotHealBtn_->setChecked(true);  // default Heal (checked before connect)
    connect(spotHealBtn_, &QPushButton::toggled, this,
            [this](bool c) { onSpotModeToggled(lrc::SpotType::Heal, c); });
    connect(spotCloneBtn_, &QPushButton::toggled, this,
            [this](bool c) { onSpotModeToggled(lrc::SpotType::Clone, c); });

    // Radius slider: normalized 0.001..0.2 (fraction of the longer edge),
    // default 0.01, with a numeric readout in the spin box.
    auto* spotRadiusRow = new QHBoxLayout;
    spotRadiusRow->setContentsMargins(0, 0, 0, 0);
    spotRadiusRow->setSpacing(6);
    auto* spotRadiusLabel = new QLabel(tr("半径"), this);
    spotRadiusLabel->setFixedWidth(46);
    spotRadiusSlider_ = new QSlider(Qt::Horizontal, this);
    spotRadiusSlider_->setRange(0, 10000);
    spotRadiusSlider_->setSingleStep(100);
    spotRadiusSpin_ = new QDoubleSpinBox(this);
    spotRadiusSpin_->setDecimals(3);
    spotRadiusSpin_->setRange(kSpotRadiusMin, kSpotRadiusMax);
    spotRadiusSpin_->setSingleStep(0.001);
    spotRadiusRow->addWidget(spotRadiusLabel);
    spotRadiusRow->addWidget(spotRadiusSlider_, 1);
    spotRadiusRow->addWidget(spotRadiusSpin_);
    spotSec->addLayout(spotRadiusRow);
    connect(spotRadiusSlider_, &QSlider::valueChanged, this,
            &DevelopPanel::onSpotRadiusSliderChanged);
    connect(spotRadiusSpin_,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &DevelopPanel::onSpotRadiusSpinChanged);
    syncSpotRadius(0.01);  // default 0.01 (blocked, no signal emission).

    spotCountLabel_ = new QLabel(tr("污点数: 0"), this);
    spotCountLabel_->setStyleSheet("color: #888888;");
    spotSec->addWidget(spotCountLabel_);

    // Visualize spots: temporary invert + high-contrast preview toggle.
    spotVisualizeBtn_ = new QPushButton(tr("可视化污点"), this);
    spotVisualizeBtn_->setCheckable(true);
    spotVisualizeBtn_->setProperty("secondary", true);
    spotSec->addWidget(spotVisualizeBtn_);
    connect(spotVisualizeBtn_, &QPushButton::toggled, this,
            &DevelopPanel::onSpotVisualizeToggled);

    // ---- 元数据 (IPTC / metadata, M8) --------------------------------------
    PanelSection* metaSec = addSection(tr("元数据"));

    auto addFieldRow = [&](const char* label, QLineEdit*& edit) {
        auto* h = new QHBoxLayout;
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(6);
        auto* lbl = new QLabel(tr(label), this);
        lbl->setFixedWidth(46);
        edit = new QLineEdit(this);
        h->addWidget(lbl);
        h->addWidget(edit, 1);
        metaSec->addLayout(h);
    };

    // Single-line fields commit on Enter / focus loss (editingFinished).
    addFieldRow("标题", metaTitleEdit_);
    connect(metaTitleEdit_, &QLineEdit::editingFinished, this,
            &DevelopPanel::emitMetadataEdited);

    // Caption: multi-line. Debounced while typing; focus-out flushes now.
    auto* captionRow = new QHBoxLayout;
    captionRow->setContentsMargins(0, 0, 0, 0);
    captionRow->setSpacing(6);
    auto* captionLabel = new QLabel(tr("说明"), this);
    captionLabel->setFixedWidth(46);
    captionLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    metaCaptionEdit_ = new QPlainTextEdit(this);
    metaCaptionEdit_->setFixedHeight(64);
    captionRow->addWidget(captionLabel);
    captionRow->addWidget(metaCaptionEdit_, 1);
    metaSec->addLayout(captionRow);
    connect(metaCaptionEdit_, &QPlainTextEdit::textChanged, this,
            &DevelopPanel::scheduleMetadataCommit);
    metaCaptionEdit_->installEventFilter(this);

    addFieldRow("版权", metaCopyrightEdit_);
    connect(metaCopyrightEdit_, &QLineEdit::editingFinished, this,
            &DevelopPanel::emitMetadataEdited);

    addFieldRow("创作者", metaCreatorEdit_);
    connect(metaCreatorEdit_, &QLineEdit::editingFinished, this,
            &DevelopPanel::emitMetadataEdited);

    // Keywords: TagInput-style -- a QLineEdit where Enter adds a removable
    // chip. Chosen over a comma-separated text box because the current set is
    // always visible and removable (matching LR's keyword tag field) and the
    // chip list round-trips cleanly through setMetadata on photo switch.
    auto* keywordRow = new QHBoxLayout;
    keywordRow->setContentsMargins(0, 0, 0, 0);
    keywordRow->setSpacing(6);
    auto* keywordLabel = new QLabel(tr("关键字"), this);
    keywordLabel->setFixedWidth(46);
    keywordInput_ = new QLineEdit(this);
    keywordInput_->setPlaceholderText(tr("输入关键字后回车"));
    keywordRow->addWidget(keywordLabel);
    keywordRow->addWidget(keywordInput_, 1);
    metaSec->addLayout(keywordRow);
    connect(keywordInput_, &QLineEdit::returnPressed, this, [this]() {
        addKeyword(keywordInput_->text());
        keywordInput_->clear();
    });

    keywordChips_ = new QWidget(this);
    keywordChipsLayout_ = new QVBoxLayout(keywordChips_);
    keywordChipsLayout_->setContentsMargins(0, 0, 0, 0);
    keywordChipsLayout_->setSpacing(2);
    keywordChips_->setVisible(false);
    metaSec->addWidget(keywordChips_);

    root->addStretch(1);

    debounce_.setSingleShot(true);
    debounce_.setInterval(lrc::app::throttle::kSliderDebounceMs);
    connect(&debounce_, &QTimer::timeout, this, &DevelopPanel::applyDebounce);

    coarseDebounce_.setSingleShot(true);
    coarseDebounce_.setInterval(lrc::app::throttle::kSliderCoarseDebounceMs);
    connect(&coarseDebounce_, &QTimer::timeout, this,
            &DevelopPanel::applyCoarseDebounce);

    // M11: mask-stroke geometry commit throttle (leading edge -- started only
    // when not already armed, so continuous moves coalesce to ~one commit per
    // interval instead of one per move).
    maskGeometryCommitTimer_.setSingleShot(true);
    maskGeometryCommitTimer_.setInterval(lrc::app::throttle::kMaskGeometryCommitMs);
    connect(&maskGeometryCommitTimer_, &QTimer::timeout, this,
            &DevelopPanel::flushMaskGeometryCommit);

    // M8: metadata edits debounce while the user types a caption (line edits
    // and focus-out commit immediately via emitMetadataEdited).
    metadataCommit_.setSingleShot(true);
    metadataCommit_.setInterval(lrc::app::throttle::kMetadataCommitMs);
    connect(&metadataCommit_, &QTimer::timeout, this,
            &DevelopPanel::emitMetadataEdited);
}

void DevelopPanel::addRow(Row row) {
    const size_t index = rows_.size();
    rows_.push_back(row);
    Row& r = rows_[index];

    connect(r.slider, &QSlider::valueChanged, this,
            [this, index](int v) {
                if (index < rows_.size())
                    onSliderChanged(rows_[index], v);
            });
    connect(r.slider, &QSlider::sliderReleased, this, [this, index]() {
        if (!suppressSignals_ && index < rows_.size())
            emit adjustmentsCommitted(current_);
        // Restore the normal preview once the Alt-drag ends.
        if (vizActive_)
            clearAltVisualization();
    });
    connect(r.spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, index](double v) {
                if (index < rows_.size())
                    onSpinChanged(rows_[index], v);
            });
    connect(r.spin, &QDoubleSpinBox::editingFinished, this,
            [this, index]() {
                if (!suppressSignals_ && index < rows_.size())
                    emit adjustmentsCommitted(current_);
            });
    connectSliderGesture(r.slider);
    syncRow(rows_[index]);
}

void DevelopPanel::connectSliderGesture(QSlider* slider) {
    // Connects a slider's press/release to the edit-gesture signals so
    // MainWindow can push one undo-stack entry per drag gesture.
    connect(slider, &QSlider::sliderPressed, this, [this]() {
        dragging_ = true;        // during a drag scheduleDebounce emits coarse
        emit editGestureStarted();
    });
    connect(slider, &QSlider::sliderReleased, this, [this]() {
        dragging_ = false;       // on release fall back to the refine path
        emit editGestureFinished();
        scheduleDebounce();
    });
}

void DevelopPanel::addHSLRow(int colorIndex, HSLRow row) {
    const size_t idx = hslRows_[static_cast<size_t>(colorIndex)].size();
    hslRows_[static_cast<size_t>(colorIndex)].push_back(row);
    HSLRow& r = hslRows_[static_cast<size_t>(colorIndex)][idx];

    const int ci = colorIndex;
    connect(r.slider, &QSlider::valueChanged, this,
            [this, ci, idx](int v) {
                if (ci < static_cast<int>(hslRows_.size()) &&
                    idx < hslRows_[static_cast<size_t>(ci)].size())
                    onHSLSliderChanged(ci, hslRows_[static_cast<size_t>(ci)][idx], v);
            });
    connect(r.slider, &QSlider::sliderReleased, this, [this]() {
        if (!suppressSignals_)
            emit adjustmentsCommitted(current_);
    });
    connect(r.spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, ci, idx](double v) {
                if (ci < static_cast<int>(hslRows_.size()) &&
                    idx < hslRows_[static_cast<size_t>(ci)].size())
                    onHSLSpinChanged(ci, hslRows_[static_cast<size_t>(ci)][idx], v);
            });
    connect(r.spin, &QDoubleSpinBox::editingFinished, this, [this]() {
        if (!suppressSignals_)
            emit adjustmentsCommitted(current_);
    });
    connectSliderGesture(r.slider);
    syncHSLRow(colorIndex, r);
}

void DevelopPanel::addSplitToneRow(SplitToneRow row) {
    const size_t index = splitToneRows_.size();
    splitToneRows_.push_back(row);
    SplitToneRow& r = splitToneRows_[index];

    connect(r.slider, &QSlider::valueChanged, this,
            [this, index](int v) {
                if (index < splitToneRows_.size())
                    onSplitToneSliderChanged(splitToneRows_[index], v);
            });
    connect(r.slider, &QSlider::sliderReleased, this, [this]() {
        if (!suppressSignals_)
            emit adjustmentsCommitted(current_);
    });
    connect(r.spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, index](double v) {
                if (index < splitToneRows_.size())
                    onSplitToneSpinChanged(splitToneRows_[index], v);
            });
    connect(r.spin, &QDoubleSpinBox::editingFinished, this, [this]() {
        if (!suppressSignals_)
            emit adjustmentsCommitted(current_);
    });
    connectSliderGesture(r.slider);
    syncSplitToneRow(r);
}

// --- masked local adjustments ------------------------------------------------

void DevelopPanel::addMaskRow(MaskRow row) {
    const size_t index = maskRows_.size();
    maskRows_.push_back(row);
    MaskRow& r = maskRows_[index];

    connect(r.slider, &QSlider::valueChanged, this,
            [this, index](int v) {
                if (index < maskRows_.size())
                    onMaskSliderChanged(maskRows_[index], v);
            });
    connect(r.slider, &QSlider::sliderReleased, this, [this]() {
        if (!suppressSignals_)
            emit adjustmentsCommitted(current_);
    });
    connect(r.spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, index](double v) {
                if (index < maskRows_.size())
                    onMaskSpinChanged(maskRows_[index], v);
            });
    connect(r.spin, &QDoubleSpinBox::editingFinished, this, [this]() {
        if (!suppressSignals_)
            emit adjustmentsCommitted(current_);
    });
    connectSliderGesture(r.slider);
    syncMaskRow(r);
}

void DevelopPanel::onMaskSliderChanged(const MaskRow& row, int value) {
    if (suppressSignals_)
        return;
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    const double v = static_cast<double>(value) / 100.0;
    if (!qFuzzyCompare(row.spin->value(), v)) {
        const QSignalBlocker blocker(row.spin);
        row.spin->setValue(v);
    }
    current_.masks[static_cast<size_t>(idx)].*row.field = v;
    scheduleDebounce();
    // QSlider keyboard edits (arrows/PageUp/Home/End) never release the mouse,
    // so persist + push one undo entry here as well. While dragging (dragging_
    // true) the sliderReleased path owns the commit.
    if (!dragging_)
        emit adjustmentsCommitted(current_);
}

void DevelopPanel::onMaskSpinChanged(const MaskRow& row, double value) {
    if (suppressSignals_)
        return;
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    const int sliderValue = static_cast<int>(std::lround(value * 100.0));
    if (row.slider->value() != sliderValue) {
        const QSignalBlocker blocker(row.slider);
        row.slider->setValue(sliderValue);
    }
    current_.masks[static_cast<size_t>(idx)].*row.field = value;
    scheduleDebounce();
}

int DevelopPanel::currentMaskIndex() const {
    return maskCombo_ ? maskCombo_->currentIndex() : -1;
}

void DevelopPanel::armMaskTool(lrc::MaskType tool) {
    QPushButton* target = nullptr;
    switch (tool) {
        case lrc::MaskType::LinearGradient:
            target = maskLinearBtn_;
            break;
        case lrc::MaskType::RadialGradient:
            target = maskRadialBtn_;
            break;
        case lrc::MaskType::Brush:
            target = maskBrushBtn_;
            break;
        case lrc::MaskType::ColorRange:
            target = maskColorBtn_;
            break;
        default:
            break;
    }
    if (!target)
        return;
    const QSignalBlocker b1(maskLinearBtn_);
    const QSignalBlocker b2(maskRadialBtn_);
    const QSignalBlocker b3(maskBrushBtn_);
    const QSignalBlocker b4(maskColorBtn_);
    maskLinearBtn_->setChecked(target == maskLinearBtn_);
    maskRadialBtn_->setChecked(target == maskRadialBtn_);
    maskBrushBtn_->setChecked(target == maskBrushBtn_);
    maskColorBtn_->setChecked(target == maskColorBtn_);
}

void DevelopPanel::uncheckMaskTools() {
    const QSignalBlocker b1(maskLinearBtn_);
    const QSignalBlocker b2(maskRadialBtn_);
    const QSignalBlocker b3(maskBrushBtn_);
    const QSignalBlocker b4(maskColorBtn_);
    maskLinearBtn_->setChecked(false);
    maskRadialBtn_->setChecked(false);
    maskBrushBtn_->setChecked(false);
    maskColorBtn_->setChecked(false);
}

void DevelopPanel::onMaskToolToggled(lrc::MaskType tool, bool checked) {
    if (!checked) {
        if (activeMaskTool_ == tool) {
            activeMaskTool_ = lrc::MaskType::None;
            emit maskEditRequested(lrc::Mask{}, -1);
        }
        return;
    }
    activateMaskTool(tool);
}

void DevelopPanel::activateMaskTool(lrc::MaskType tool) {
    // Arming a tool always creates a fresh mask of that type (LR behaviour).
    lrc::Mask m;
    switch (tool) {
        case lrc::MaskType::ColorRange:
            // Fresh colour-range mask: type set, reference absent (inert until
            // a colour is sampled).
            m.type = lrc::MaskType::ColorRange;
            m.color = lrc::ColorRangeMask{};
            break;
        default:
            m.type = tool;
            break;
    }
    current_.masks.push_back(m);
    activeMaskTool_ = tool;
    const int index = static_cast<int>(current_.masks.size()) - 1;
    syncMaskSection();
    {
        const QSignalBlocker b(maskCombo_);
        maskCombo_->setCurrentIndex(index);
    }
    updateMaskSectionVisibility();
    syncMaskSliders();
    if (tool == lrc::MaskType::Brush) {
        emit maskBrushSizeChanged(
            static_cast<double>(maskBrushSizeSlider_->value()) / 20000.0);
        emit maskSoftnessChanged(
            static_cast<double>(maskSoftnessSlider_->value()) / 10000.0);
    }
    emit maskEditRequested(current_.masks[static_cast<size_t>(index)], index);
    // Every mask-tool activation funnels through here (panel buttons, the
    // selectMaskTool slot), so this is the single point that mirrors the
    // active tool to MainWindow's toolbar.
    emit maskToolActivated(tool);
}

void DevelopPanel::addAiMask(lrc::AiMaskTarget target) {
    // One-shot AI mask creation (no interactive drawing step). The mask's
    // weight is rasterized inside Pipeline::applyLocalAdjustments via
    // AiMaskGenerator::generate (heuristicFallback when the model is
    // unavailable). cachedWeightVersion starts at 0; bumping it later
    // forces Pipeline to regenerate the segmentation.
    lrc::Mask m;
    m.type = lrc::MaskType::AiMask;
    m.ai.target = target;
    m.ai.threshold = 0.5f;
    m.ai.feather = 0.05f;
    m.ai.invert = false;
    m.ai.modelInputSize = 1024;
    m.ai.cachedWeightVersion = 1;  // >0: distinct from a default-constructed
                                   // mask so geometryHash treats it as set
    current_.masks.push_back(m);
    activeMaskTool_ = lrc::MaskType::AiMask;
    const int index = static_cast<int>(current_.masks.size()) - 1;
    syncMaskSection();
    {
        const QSignalBlocker b(maskCombo_);
        maskCombo_->setCurrentIndex(index);
    }
    updateMaskSectionVisibility();
    syncMaskSliders();
    // No maskEditRequested: AiMask has no on-canvas geometry to edit. The
    // preview still re-renders because scheduleDebounce fires.
    emit maskToolActivated(lrc::MaskType::AiMask);
    scheduleDebounce();
}

void DevelopPanel::selectMaskTool(lrc::MaskType tool) {
    if (tool == lrc::MaskType::None) {
        activeMaskTool_ = lrc::MaskType::None;
        uncheckMaskTools();
        emit maskEditRequested(lrc::Mask{}, -1);
        emit maskToolActivated(lrc::MaskType::None);
        return;
    }
    if (tool == activeMaskTool_)
        return;  // 已在同一工具：幂等，避免重复创建空蒙版 (idempotent)
    armMaskTool(tool);
    activateMaskTool(tool);
}

void DevelopPanel::onMaskSelected(int index) {
    if (index < 0 || index >= static_cast<int>(current_.masks.size())) {
        activeMaskTool_ = lrc::MaskType::None;
        uncheckMaskTools();
        updateMaskSectionVisibility();
        syncMaskSliders();
        emit maskEditRequested(lrc::Mask{}, -1);
        emit maskToolActivated(lrc::MaskType::None);
        return;
    }
    const lrc::Mask& m = current_.masks[static_cast<size_t>(index)];
    if (m.type != lrc::MaskType::None) {
        armMaskTool(m.type);
        activeMaskTool_ = m.type;
        if (m.type == lrc::MaskType::Brush) {
            emit maskBrushSizeChanged(
                static_cast<double>(maskBrushSizeSlider_->value()) / 20000.0);
            emit maskSoftnessChanged(
                static_cast<double>(maskSoftnessSlider_->value()) / 10000.0);
        }
        // Selecting an existing mask arms its tool directly without creating a
        // new one, so this path skips activateMaskTool -- mirror it as well.
        emit maskToolActivated(m.type);
    } else {
        activeMaskTool_ = lrc::MaskType::None;
        uncheckMaskTools();
        emit maskToolActivated(lrc::MaskType::None);
    }
    updateMaskSectionVisibility();
    syncMaskSliders();
    emit maskEditRequested(m, index);
}

void DevelopPanel::onMaskDelete() {
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    current_.masks.erase(current_.masks.begin() + idx);
    activeMaskTool_ = lrc::MaskType::None;
    uncheckMaskTools();
    syncMaskSection();
    syncMaskSliders();
    emit maskEditRequested(lrc::Mask{}, -1);
    emit maskToolActivated(lrc::MaskType::None);
    scheduleDebounce();
    emit adjustmentsCommitted(current_);
}

void DevelopPanel::onMaskOpacityChanged(int value) {
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    current_.masks[static_cast<size_t>(idx)].opacity =
        static_cast<double>(value) / 10000.0;
    scheduleDebounce();
}

void DevelopPanel::onMaskInvertToggled(bool checked) {
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    current_.masks[static_cast<size_t>(idx)].invert = checked;
    refreshMaskDraft();
    scheduleDebounce();
    // A checkbox toggle never releases a slider, so persist it explicitly
    // (previously lost on image switch).
    if (!suppressSignals_)
        emit adjustmentsCommitted(current_);
}

void DevelopPanel::onMaskFeatherChanged(int value) {
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    lrc::Mask& m = current_.masks[static_cast<size_t>(idx)];
    const float f = static_cast<float>(value) / 10000.0f;
    if (m.type == lrc::MaskType::LinearGradient)
        m.linear.feather = f;
    else if (m.type == lrc::MaskType::RadialGradient)
        m.radial.feather = f;
    else
        return;
    // Keep the slider and spin in sync (spin shows 0..100, slider 0..10000).
    {
        const QSignalBlocker b1(maskFeatherSlider_);
        const QSignalBlocker b2(maskFeatherSpin_);
        maskFeatherSlider_->setValue(value);
        maskFeatherSpin_->setValue(static_cast<double>(value) / 100.0);
    }
    refreshMaskDraft();
    scheduleDebounce();
}

void DevelopPanel::onMaskBrushSizeChanged(int value) {
    // value 0..10000 -> normalized dab radius 0..0.5 (LR brush range).
    emit maskBrushSizeChanged(static_cast<double>(value) / 20000.0);
}

void DevelopPanel::onMaskSoftnessChanged(int value) {
    // value 0..10000 -> softness 0..1 (only affects new brush dabs).
    emit maskSoftnessChanged(static_cast<double>(value) / 10000.0);
}

void DevelopPanel::changeMaskBrushSize(int direction) {
    if (activeMaskTool_ == lrc::MaskType::None || !maskBrushSizeSlider_)
        return;
    if (direction == 0)
        return;
    const double factor = (direction > 0) ? 1.1 : (1.0 / 1.1);
    const int value = maskBrushSizeSlider_->value();
    const int clamped = static_cast<int>(std::clamp(
        static_cast<double>(value) * factor,
        static_cast<double>(maskBrushSizeSlider_->minimum()),
        static_cast<double>(maskBrushSizeSlider_->maximum())));
    if (clamped != value)
        maskBrushSizeSlider_->setValue(clamped);
}

void DevelopPanel::adjustExposure(double deltaEv) {
    for (const Row& row : rows_) {
        if (row.field != &lrc::Adjustments::exposure)
            continue;
        const double v =
            std::clamp(current_.exposure + deltaEv, row.min, row.max);
        const double rounded = std::round(v * 100.0) / 100.0;
        if (!qFuzzyCompare(current_.exposure, rounded)) {
            current_.exposure = rounded;
            {
                const QSignalBlocker sb(row.slider);
                const QSignalBlocker sp(row.spin);
                row.slider->setValue(
                    static_cast<int>(std::lround(rounded * 100.0)));
                row.spin->setValue(rounded);
            }
            scheduleDebounce();
            emit adjustmentsCommitted(current_);
        }
        break;
    }
}

void DevelopPanel::refreshMaskDraft() {
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    if (activeMaskTool_ != lrc::MaskType::None)
        emit maskEditRequested(current_.masks[static_cast<size_t>(idx)], idx);
}

void DevelopPanel::updateMaskGeometry(int index, const lrc::Mask& mask) {
    if (index < 0 || index >= static_cast<int>(current_.masks.size()))
        return;
    lrc::Mask& dst = current_.masks[static_cast<size_t>(index)];
    // Copy geometry only; keep the local adjustment values + opacity that the
    // panel owns so the viewport's draft can't clobber the sliders.
    dst.type = mask.type;
    dst.linear = mask.linear;
    dst.radial = mask.radial;
    dst.brush = mask.brush;
    dst.invert = mask.invert;
    scheduleDebounce();
    // M11: coalesce the commit while the stroke is in flight. Every mouse move
    // reaches here, and emitting adjustmentsCommitted per move made MainWindow
    // re-sync the whole panel (60+ rows) and write the DB synchronously each
    // time. The leading-edge timer bounds that to about one commit per
    // app::throttle::kMaskGeometryCommitMs; the stroke-end path persists the
    // final state and setAdjustments cancels any still-pending flush, so a
    // late timer can never double-push an undo entry.
    scheduleMaskGeometryCommit();
}

void DevelopPanel::scheduleMaskGeometryCommit() {
    maskGeometryDirty_ = true;
    if (!maskGeometryCommitTimer_.isActive())
        maskGeometryCommitTimer_.start();
}

void DevelopPanel::flushMaskGeometryCommit() {
    if (!maskGeometryDirty_)
        return;
    maskGeometryDirty_ = false;
    if (!suppressSignals_)
        emit adjustmentsCommitted(current_);
}

void DevelopPanel::syncMaskRow(const MaskRow& row) {
    const QSignalBlocker s(row.slider);
    const QSignalBlocker b(row.spin);
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size())) {
        row.slider->setValue(0);
        row.spin->setValue(0);
        return;
    }
    const double v = current_.masks[static_cast<size_t>(idx)].*row.field;
    row.slider->setValue(static_cast<int>(std::lround(v * 100.0)));
    row.spin->setValue(v);
}

void DevelopPanel::syncMaskSliders() {
    const int idx = currentMaskIndex();
    const bool has = idx >= 0 && idx < static_cast<int>(current_.masks.size());
    for (const MaskRow& row : maskRows_)
        syncMaskRow(row);
    {
        const QSignalBlocker b(maskOpacitySlider_);
        maskOpacitySlider_->setValue(
            has ? static_cast<int>(std::lround(
                      current_.masks[static_cast<size_t>(idx)].opacity * 10000.0))
                : 0);
    }
    {
        const QSignalBlocker b(maskInvertCheck_);
        maskInvertCheck_->setChecked(
            has && current_.masks[static_cast<size_t>(idx)].invert);
    }
    double feather = 0.0;
    if (has) {
        const lrc::Mask& m = current_.masks[static_cast<size_t>(idx)];
        if (m.type == lrc::MaskType::LinearGradient)
            feather = m.linear.feather;
        else if (m.type == lrc::MaskType::RadialGradient)
            feather = m.radial.feather;
    }
    {
        const QSignalBlocker b1(maskFeatherSlider_);
        const QSignalBlocker b2(maskFeatherSpin_);
        maskFeatherSlider_->setValue(static_cast<int>(std::lround(feather * 10000.0)));
        maskFeatherSpin_->setValue(feather * 100.0);
    }
    for (const MaskRow& row : maskRows_) {
        row.slider->setEnabled(has);
        row.spin->setEnabled(has);
    }
    maskOpacitySlider_->setEnabled(has);
    maskInvertCheck_->setEnabled(has);
    syncColorRangeSection();
    syncSubRangeSection();
}

void DevelopPanel::syncMaskSection() {
    const int keep = currentMaskIndex();
    {
        const QSignalBlocker b(maskCombo_);
        maskCombo_->clear();
        for (size_t i = 0; i < current_.masks.size(); ++i)
            maskCombo_->addItem(tr("蒙版 %1").arg(static_cast<int>(i) + 1));
        // clear()+addItem auto-selects index 0; restore the kept selection, or
        // explicitly deselect when the tool was disarmed (keep == -1) so the
        // combo does not silently show "蒙版 1" as selected.
        if (keep >= 0 && keep < static_cast<int>(current_.masks.size()))
            maskCombo_->setCurrentIndex(keep);
        else
            maskCombo_->setCurrentIndex(-1);
    }
    updateMaskSectionVisibility();
}

void DevelopPanel::updateMaskSectionVisibility() {
    const int idx = currentMaskIndex();
    const bool has = idx >= 0 && idx < static_cast<int>(current_.masks.size());
    bool showFeather = false, showBrush = false, showColor = false,
         showSubRange = false;
    if (has) {
        const lrc::MaskType t = current_.masks[static_cast<size_t>(idx)].type;
        showFeather = (t == lrc::MaskType::LinearGradient ||
                       t == lrc::MaskType::RadialGradient);
        showBrush = (t == lrc::MaskType::Brush);
        showColor = (t == lrc::MaskType::ColorRange);
        // PHASE_4: the range sub-mask panel layers on top of geometry masks
        // (linear / radial / brush), not the standalone colour-range mask.
        showSubRange = has && t != lrc::MaskType::None &&
                       t != lrc::MaskType::ColorRange;
    }
    maskFeatherRow_->setVisible(showFeather);
    maskBrushRow_->setVisible(showBrush);
    maskSoftnessRow_->setVisible(showBrush);
    maskColorRow_->setVisible(showColor);
    subRangeRow_->setVisible(showSubRange);
    maskDeleteBtn_->setEnabled(has);
}

void DevelopPanel::clearMaskTool() {
    activeMaskTool_ = lrc::MaskType::None;
    uncheckMaskTools();
    {
        const QSignalBlocker b(maskCombo_);
        maskCombo_->setCurrentIndex(-1);
    }
    syncMaskSection();
    syncMaskSliders();
    emit maskToolActivated(lrc::MaskType::None);
}

// --- colour-range mask --------------------------------------------------------

void DevelopPanel::onColorRangeToleranceSlider(int value) {
    if (suppressSignals_)
        return;
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    lrc::Mask& m = current_.masks[static_cast<size_t>(idx)];
    if (m.type != lrc::MaskType::ColorRange)
        return;

    QDoubleSpinBox* spin = nullptr;
    if (sender() == colorHueSlider_) {
        m.color.hueTolerance = static_cast<double>(value) / 100.0;  // degrees.
        spin = colorHueSpin_;
    } else if (sender() == colorSatSlider_) {
        m.color.satTolerance = static_cast<double>(value) / 10000.0;
        spin = colorSatSpin_;
    } else if (sender() == colorLumaSlider_) {
        m.color.lumaTolerance = static_cast<double>(value) / 10000.0;
        spin = colorLumaSpin_;
    } else if (sender() == colorSoftnessSlider_) {
        m.color.softness = static_cast<double>(value) / 10000.0;
        spin = colorSoftnessSpin_;
    } else {
        return;
    }

    // makeSliderRow keeps the slider at 100x the spin: the spin shows hue
    // degrees (0..180) or percentage (0..100) for the sat/luma/softness rows.
    if (spin) {
        const QSignalBlocker blocker(spin);
        spin->setValue(static_cast<double>(value) / 100.0);
    }
    refreshMaskDraft();
    scheduleDebounce();
    // Keyboard edits never release the slider, so commit here too (keyboard).
    if (!dragging_)
        emit adjustmentsCommitted(current_);
}

void DevelopPanel::onColorRangeToleranceSpin(double value) {
    if (suppressSignals_)
        return;
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    lrc::Mask& m = current_.masks[static_cast<size_t>(idx)];
    if (m.type != lrc::MaskType::ColorRange)
        return;

    QSlider* slider = nullptr;
    if (sender() == colorHueSpin_) {
        m.color.hueTolerance = value;  // degrees.
        slider = colorHueSlider_;
    } else if (sender() == colorSatSpin_) {
        m.color.satTolerance = value / 100.0;
        slider = colorSatSlider_;
    } else if (sender() == colorLumaSpin_) {
        m.color.lumaTolerance = value / 100.0;
        slider = colorLumaSlider_;
    } else if (sender() == colorSoftnessSpin_) {
        m.color.softness = value / 100.0;
        slider = colorSoftnessSlider_;
    } else {
        return;
    }

    if (slider) {
        const QSignalBlocker blocker(slider);
        slider->setValue(static_cast<int>(std::lround(value * 100.0)));
    }
    refreshMaskDraft();
    scheduleDebounce();
}

void DevelopPanel::onColorRangeSampleToggled(bool active) {
    if (active) {
        // 与范围子蒙版吸管互斥 (mutual exclusion with the sub-mask samplers).
        const QSignalBlocker b1(subColorSampleBtn_);
        const QSignalBlocker b2(subLumaSampleBtn_);
        subColorSampleBtn_->setChecked(false);
        subLumaSampleBtn_->setChecked(false);
    }
    emit colorRangeSamplerRequested(active);
}

void DevelopPanel::syncColorRangeSection() {
    const int idx = currentMaskIndex();
    const bool has = idx >= 0 && idx < static_cast<int>(current_.masks.size());
    lrc::ColorRangeMask color;   // defaults when nothing is selected.
    bool isColorRange = false;
    if (has) {
        const lrc::Mask& m = current_.masks[static_cast<size_t>(idx)];
        isColorRange = (m.type == lrc::MaskType::ColorRange);
        if (isColorRange)
            color = m.color;
    }

    const double hue = color.hueTolerance;          // degrees.
    const double sat = color.satTolerance * 100.0;  // percentage.
    const double luma = color.lumaTolerance * 100.0;
    const double soft = color.softness * 100.0;
    {
        const QSignalBlocker b1(colorHueSlider_);
        const QSignalBlocker b2(colorHueSpin_);
        colorHueSlider_->setValue(static_cast<int>(std::lround(hue * 100.0)));
        colorHueSpin_->setValue(hue);
    }
    {
        const QSignalBlocker b1(colorSatSlider_);
        const QSignalBlocker b2(colorSatSpin_);
        colorSatSlider_->setValue(static_cast<int>(std::lround(sat * 100.0)));
        colorSatSpin_->setValue(sat);
    }
    {
        const QSignalBlocker b1(colorLumaSlider_);
        const QSignalBlocker b2(colorLumaSpin_);
        colorLumaSlider_->setValue(static_cast<int>(std::lround(luma * 100.0)));
        colorLumaSpin_->setValue(luma);
    }
    {
        const QSignalBlocker b1(colorSoftnessSlider_);
        const QSignalBlocker b2(colorSoftnessSpin_);
        colorSoftnessSlider_->setValue(static_cast<int>(std::lround(soft * 100.0)));
        colorSoftnessSpin_->setValue(soft);
    }

    // Tolerance rows are usable only once a reference colour has been sampled;
    // the sample button is available for any selected colour-range mask.
    const bool active = has && isColorRange && color.hasReference;
    colorHueSlider_->setEnabled(active);
    colorHueSpin_->setEnabled(active);
    colorSatSlider_->setEnabled(active);
    colorSatSpin_->setEnabled(active);
    colorLumaSlider_->setEnabled(active);
    colorLumaSpin_->setEnabled(active);
    colorSoftnessSlider_->setEnabled(active);
    colorSoftnessSpin_->setEnabled(active);
    colorSampleBtn_->setEnabled(has && isColorRange);
    updateColorRangeSwatch(color.hasReference
                               ? linearToSwatch(color.refR, color.refG, color.refB)
                               : QColor());
}

void DevelopPanel::updateColorRangeSwatch(const QColor& color) {
    if (!colorRefSwatch_)
        return;
    if (!color.isValid()) {
        colorRefSwatch_->setStyleSheet(
            "background-color: #2a2a2a; border: 1px solid #666666; "
            "border-radius: 3px;");
        return;
    }
    colorRefSwatch_->setStyleSheet(
        QString("background-color: %1; border: 1px solid #666666; "
                "border-radius: 3px;").arg(color.name()));
}

void DevelopPanel::setColorRangeReference(const QColor& srgb) {
    // UI-only: reflect the sampled sRGB colour in the swatch and enable the
    // tolerance rows. The reference data (refR/G/B, hasReference) is written
    // into the model by MainWindow's onColorRangeSampled handler.
    updateColorRangeSwatch(srgb);
    const int idx = currentMaskIndex();
    const bool active = srgb.isValid() &&
                        idx >= 0 &&
                        idx < static_cast<int>(current_.masks.size()) &&
                        current_.masks[static_cast<size_t>(idx)].type ==
                            lrc::MaskType::ColorRange;
    colorHueSlider_->setEnabled(active);
    colorHueSpin_->setEnabled(active);
    colorSatSlider_->setEnabled(active);
    colorSatSpin_->setEnabled(active);
    colorLumaSlider_->setEnabled(active);
    colorLumaSpin_->setEnabled(active);
    colorSoftnessSlider_->setEnabled(active);
    colorSoftnessSpin_->setEnabled(active);
}

// --- range sub-mask (colour / luminance) (PHASE_4) ---------------------------

// True when `m` is a geometry mask (linear / radial / brush) that can carry the
// colour/luminance sub-mask modifiers.
bool DevelopPanel::isGeometryMask(const lrc::Mask& m) {
    return m.type != lrc::MaskType::None &&
           m.type != lrc::MaskType::ColorRange;
}

void DevelopPanel::onSubColorEnableToggled(bool checked) {
    if (suppressSignals_)
        return;
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    lrc::Mask& m = current_.masks[static_cast<size_t>(idx)];
    if (!isGeometryMask(m))
        return;
    m.useColorRange = checked;
    syncSubRangeSection();
    refreshMaskDraft();
    scheduleDebounce();
    if (!suppressSignals_)
        emit adjustmentsCommitted(current_);
}

void DevelopPanel::onSubColorRangeSlider(int value) {
    if (suppressSignals_)
        return;
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    lrc::Mask& m = current_.masks[static_cast<size_t>(idx)];
    if (!isGeometryMask(m))
        return;
    const double v = static_cast<double>(value) / 100.0;
    if (!qFuzzyCompare(subColorRangeSpin_->value(), v)) {
        const QSignalBlocker blocker(subColorRangeSpin_);
        subColorRangeSpin_->setValue(v);
    }
    m.colorRange = static_cast<float>(v);
    refreshMaskDraft();
    scheduleDebounce();
    if (!dragging_)
        emit adjustmentsCommitted(current_);
}

void DevelopPanel::onSubColorRangeSpin(double value) {
    if (suppressSignals_)
        return;
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    lrc::Mask& m = current_.masks[static_cast<size_t>(idx)];
    if (!isGeometryMask(m))
        return;
    const int sliderValue = static_cast<int>(std::lround(value * 100.0));
    if (subColorRangeSlider_->value() != sliderValue) {
        const QSignalBlocker blocker(subColorRangeSlider_);
        subColorRangeSlider_->setValue(sliderValue);
    }
    m.colorRange = static_cast<float>(value);
    refreshMaskDraft();
    scheduleDebounce();
}

void DevelopPanel::onSubColorSmoothSlider(int value) {
    if (suppressSignals_)
        return;
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    lrc::Mask& m = current_.masks[static_cast<size_t>(idx)];
    if (!isGeometryMask(m))
        return;
    const double v = static_cast<double>(value) / 100.0;
    if (!qFuzzyCompare(subColorSmoothSpin_->value(), v)) {
        const QSignalBlocker blocker(subColorSmoothSpin_);
        subColorSmoothSpin_->setValue(v);
    }
    m.colorSmoothness = static_cast<float>(v);
    refreshMaskDraft();
    scheduleDebounce();
    if (!dragging_)
        emit adjustmentsCommitted(current_);
}

void DevelopPanel::onSubColorSmoothSpin(double value) {
    if (suppressSignals_)
        return;
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    lrc::Mask& m = current_.masks[static_cast<size_t>(idx)];
    if (!isGeometryMask(m))
        return;
    const int sliderValue = static_cast<int>(std::lround(value * 100.0));
    if (subColorSmoothSlider_->value() != sliderValue) {
        const QSignalBlocker blocker(subColorSmoothSlider_);
        subColorSmoothSlider_->setValue(sliderValue);
    }
    m.colorSmoothness = static_cast<float>(value);
    refreshMaskDraft();
    scheduleDebounce();
}

void DevelopPanel::onSubColorSampleToggled(bool active) {
    if (active) {
        // 与明亮度/独立颜色范围吸管互斥 (mutual exclusion).
        const QSignalBlocker b1(subLumaSampleBtn_);
        const QSignalBlocker b2(colorSampleBtn_);
        subLumaSampleBtn_->setChecked(false);
        colorSampleBtn_->setChecked(false);
    }
    emit colorRangeSubSamplerRequested(active);
}

void DevelopPanel::onSubLumaEnableToggled(bool checked) {
    if (suppressSignals_)
        return;
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    lrc::Mask& m = current_.masks[static_cast<size_t>(idx)];
    if (!isGeometryMask(m))
        return;
    m.useLuminance = checked;
    syncSubRangeSection();
    refreshMaskDraft();
    scheduleDebounce();
    if (!suppressSignals_)
        emit adjustmentsCommitted(current_);
}

void DevelopPanel::onSubLumaRangeSlider(int value) {
    if (suppressSignals_)
        return;
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    lrc::Mask& m = current_.masks[static_cast<size_t>(idx)];
    if (!isGeometryMask(m))
        return;
    const double v = static_cast<double>(value) / 100.0;
    if (!qFuzzyCompare(subLumaRangeSpin_->value(), v)) {
        const QSignalBlocker blocker(subLumaRangeSpin_);
        subLumaRangeSpin_->setValue(v);
    }
    m.luminanceRange = static_cast<float>(v);
    refreshMaskDraft();
    scheduleDebounce();
    if (!dragging_)
        emit adjustmentsCommitted(current_);
}

void DevelopPanel::onSubLumaRangeSpin(double value) {
    if (suppressSignals_)
        return;
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    lrc::Mask& m = current_.masks[static_cast<size_t>(idx)];
    if (!isGeometryMask(m))
        return;
    const int sliderValue = static_cast<int>(std::lround(value * 100.0));
    if (subLumaRangeSlider_->value() != sliderValue) {
        const QSignalBlocker blocker(subLumaRangeSlider_);
        subLumaRangeSlider_->setValue(sliderValue);
    }
    m.luminanceRange = static_cast<float>(value);
    refreshMaskDraft();
    scheduleDebounce();
}

void DevelopPanel::onSubLumaSmoothSlider(int value) {
    if (suppressSignals_)
        return;
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    lrc::Mask& m = current_.masks[static_cast<size_t>(idx)];
    if (!isGeometryMask(m))
        return;
    const double v = static_cast<double>(value) / 100.0;
    if (!qFuzzyCompare(subLumaSmoothSpin_->value(), v)) {
        const QSignalBlocker blocker(subLumaSmoothSpin_);
        subLumaSmoothSpin_->setValue(v);
    }
    m.luminanceSmoothness = static_cast<float>(v);
    refreshMaskDraft();
    scheduleDebounce();
    if (!dragging_)
        emit adjustmentsCommitted(current_);
}

void DevelopPanel::onSubLumaSmoothSpin(double value) {
    if (suppressSignals_)
        return;
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    lrc::Mask& m = current_.masks[static_cast<size_t>(idx)];
    if (!isGeometryMask(m))
        return;
    const int sliderValue = static_cast<int>(std::lround(value * 100.0));
    if (subLumaSmoothSlider_->value() != sliderValue) {
        const QSignalBlocker blocker(subLumaSmoothSlider_);
        subLumaSmoothSlider_->setValue(sliderValue);
    }
    m.luminanceSmoothness = static_cast<float>(value);
    refreshMaskDraft();
    scheduleDebounce();
}

void DevelopPanel::onSubLumaSampleToggled(bool active) {
    if (active) {
        // 与颜色范围/独立颜色范围吸管互斥 (mutual exclusion).
        const QSignalBlocker b1(subColorSampleBtn_);
        const QSignalBlocker b2(colorSampleBtn_);
        subColorSampleBtn_->setChecked(false);
        colorSampleBtn_->setChecked(false);
    }
    emit luminanceSamplerRequested(active);
}

void DevelopPanel::onSubUnionChanged(int index) {
    if (suppressSignals_)
        return;
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    lrc::Mask& m = current_.masks[static_cast<size_t>(idx)];
    if (!isGeometryMask(m))
        return;
    m.maskUnion = (index == 1);
    refreshMaskDraft();
    scheduleDebounce();
    if (!dragging_)
        emit adjustmentsCommitted(current_);
}

void DevelopPanel::syncSubRangeSection() {
    const int idx = currentMaskIndex();
    const bool has = idx >= 0 && idx < static_cast<int>(current_.masks.size());
    lrc::Mask m;   // defaults when nothing is selected.
    bool isGeo = false;
    if (has) {
        m = current_.masks[static_cast<size_t>(idx)];
        isGeo = isGeometryMask(m);
    }
    const bool colorActive = isGeo && m.useColorRange;
    const bool lumaActive = isGeo && m.useLuminance;

    {
        const QSignalBlocker b(subColorEnableCheck_);
        subColorEnableCheck_->setChecked(colorActive);
    }
    {
        const QSignalBlocker b1(subColorRangeSlider_);
        const QSignalBlocker b2(subColorRangeSpin_);
        const double v = m.colorRange;
        subColorRangeSlider_->setValue(static_cast<int>(std::lround(v * 100.0)));
        subColorRangeSpin_->setValue(v);
    }
    {
        const QSignalBlocker b1(subColorSmoothSlider_);
        const QSignalBlocker b2(subColorSmoothSpin_);
        const double v = m.colorSmoothness;
        subColorSmoothSlider_->setValue(static_cast<int>(std::lround(v * 100.0)));
        subColorSmoothSpin_->setValue(v);
    }
    {
        const QSignalBlocker b(subLumaEnableCheck_);
        subLumaEnableCheck_->setChecked(lumaActive);
    }
    {
        const QSignalBlocker b1(subLumaRangeSlider_);
        const QSignalBlocker b2(subLumaRangeSpin_);
        const double v = m.luminanceRange;
        subLumaRangeSlider_->setValue(static_cast<int>(std::lround(v * 100.0)));
        subLumaRangeSpin_->setValue(v);
    }
    {
        const QSignalBlocker b1(subLumaSmoothSlider_);
        const QSignalBlocker b2(subLumaSmoothSpin_);
        const double v = m.luminanceSmoothness;
        subLumaSmoothSlider_->setValue(static_cast<int>(std::lround(v * 100.0)));
        subLumaSmoothSpin_->setValue(v);
    }

    subColorRangeSlider_->setEnabled(colorActive);
    subColorRangeSpin_->setEnabled(colorActive);
    subColorSmoothSlider_->setEnabled(colorActive);
    subColorSmoothSpin_->setEnabled(colorActive);
    subColorSampleBtn_->setEnabled(isGeo);
    subLumaRangeSlider_->setEnabled(lumaActive);
    subLumaRangeSpin_->setEnabled(lumaActive);
    subLumaSmoothSlider_->setEnabled(lumaActive);
    subLumaSmoothSpin_->setEnabled(lumaActive);
    subLumaSampleBtn_->setEnabled(isGeo);
    subUnionCombo_->setEnabled(isGeo && (m.useColorRange || m.useLuminance));
    {
        const QSignalBlocker b(subUnionCombo_);
        subUnionCombo_->setCurrentIndex(m.maskUnion ? 1 : 0);
    }
    // Leaving the geometry-mask context disarms the sub-mask samplers so a
    // stale button check cannot leave the viewport eyedropper armed.
    if (!isGeo) {
        const QSignalBlocker b1(subColorSampleBtn_);
        const QSignalBlocker b2(subLumaSampleBtn_);
        subColorSampleBtn_->setChecked(false);
        subLumaSampleBtn_->setChecked(false);
    }
    updateSubColorSwatch(colorActive
                             ? linearToSwatch(m.colorRefR, m.colorRefG, m.colorRefB)
                             : QColor());
    updateSubLumaValueLabel(lumaActive ? m.luminanceRef : -1.0);
}

void DevelopPanel::updateSubColorSwatch(const QColor& color) {
    if (!subColorSwatch_)
        return;
    if (!color.isValid()) {
        subColorSwatch_->setStyleSheet(
            "background-color: #2a2a2a; border: 1px solid #666666; "
            "border-radius: 3px;");
        return;
    }
    subColorSwatch_->setStyleSheet(
        QString("background-color: %1; border: 1px solid #666666; "
                "border-radius: 3px;").arg(color.name()));
}

void DevelopPanel::updateSubLumaValueLabel(double luma) {
    if (!subLumaValueLabel_)
        return;
    if (luma < 0.0) {
        subLumaValueLabel_->setText(tr("参考: --"));
        return;
    }
    subLumaValueLabel_->setText(tr("参考: %1").arg(luma, 0, 'f', 1));
}

void DevelopPanel::setColorRangeSubReference(const QColor& srgb, float lr,
                                             float lg, float lb) {
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    lrc::Mask& m = current_.masks[static_cast<size_t>(idx)];
    if (!isGeometryMask(m))
        return;  // 仅几何蒙版接受范围子蒙版参考色 (geometry masks only).
    m.useColorRange = true;
    m.colorRefR = lr;
    m.colorRefG = lg;
    m.colorRefB = lb;
    syncSubRangeSection();
    updateSubColorSwatch(srgb);   // exact sampled display colour.
    refreshMaskDraft();
    scheduleDebounce();
    emit adjustmentsCommitted(current_);
}

void DevelopPanel::setLuminanceSubReference(double luma) {
    const int idx = currentMaskIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.masks.size()))
        return;
    lrc::Mask& m = current_.masks[static_cast<size_t>(idx)];
    if (!isGeometryMask(m))
        return;  // 仅几何蒙版接受明亮度范围参考 (geometry masks only).
    m.useLuminance = true;
    m.luminanceRef = static_cast<float>(std::clamp(luma, 0.0, 100.0));
    syncSubRangeSection();
    refreshMaskDraft();
    scheduleDebounce();
    emit adjustmentsCommitted(current_);
}

void DevelopPanel::setRangeSubSamplerChecked(bool active) {
    const QSignalBlocker b1(subColorSampleBtn_);
    const QSignalBlocker b2(subLumaSampleBtn_);
    subColorSampleBtn_->setChecked(active);
    subLumaSampleBtn_->setChecked(active);
}

// --- point-colour (ColorTarget) targets --------------------------------------

void DevelopPanel::addColorTargetRow(ColorTargetRow row) {
    const size_t index = colorTargetRows_.size();
    colorTargetRows_.push_back(row);
    ColorTargetRow& r = colorTargetRows_[index];

    connect(r.slider, &QSlider::valueChanged, this,
            [this, index](int v) {
                if (index < colorTargetRows_.size())
                    onColorTargetSliderChanged(colorTargetRows_[index], v);
            });
    connect(r.slider, &QSlider::sliderReleased, this, [this]() {
        if (!suppressSignals_)
            emit adjustmentsCommitted(current_);
    });
    connect(r.spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, index](double v) {
                if (index < colorTargetRows_.size())
                    onColorTargetSpinChanged(colorTargetRows_[index], v);
            });
    connect(r.spin, &QDoubleSpinBox::editingFinished, this, [this]() {
        if (!suppressSignals_)
            emit adjustmentsCommitted(current_);
    });
    connectSliderGesture(r.slider);
    syncColorTargetRow(r);
}

void DevelopPanel::onColorTargetSliderChanged(const ColorTargetRow& row,
                                              int value) {
    if (suppressSignals_)
        return;
    const int idx = currentColorTargetIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.colorTargets.size()))
        return;
    const double v = static_cast<double>(value) / 100.0;
    if (!qFuzzyCompare(row.spin->value(), v)) {
        const QSignalBlocker blocker(row.spin);
        row.spin->setValue(v);
    }
    current_.colorTargets[static_cast<size_t>(idx)].*row.field = v;
    scheduleDebounce();
    // Keyboard edits never release the slider; commit here too (keyboard commit).
    if (!dragging_)
        emit adjustmentsCommitted(current_);
}

void DevelopPanel::onColorTargetSpinChanged(const ColorTargetRow& row,
                                            double value) {
    if (suppressSignals_)
        return;
    const int idx = currentColorTargetIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.colorTargets.size()))
        return;
    const int sliderValue = static_cast<int>(std::lround(value * 100.0));
    if (row.slider->value() != sliderValue) {
        const QSignalBlocker blocker(row.slider);
        row.slider->setValue(sliderValue);
    }
    current_.colorTargets[static_cast<size_t>(idx)].*row.field = value;
    scheduleDebounce();
}

int DevelopPanel::currentColorTargetIndex() const {
    return colorTargetCombo_ ? colorTargetCombo_->currentIndex() : -1;
}

void DevelopPanel::onColorTargetSelected(int index) {
    Q_UNUSED(index);
    syncColorTargetSliders();
}

void DevelopPanel::onColorTargetAdd() {
    current_.colorTargets.push_back(lrc::ColorTarget{});
    const int index = static_cast<int>(current_.colorTargets.size()) - 1;
    syncColorTargetSection();
    {
        const QSignalBlocker b(colorTargetCombo_);
        colorTargetCombo_->setCurrentIndex(index);
    }
    syncColorTargetSliders();
    emit adjustmentsCommitted(current_);
}

void DevelopPanel::onColorTargetDelete() {
    const int idx = currentColorTargetIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.colorTargets.size()))
        return;
    current_.colorTargets.erase(current_.colorTargets.begin() + idx);
    syncColorTargetSection();
    syncColorTargetSliders();
    scheduleDebounce();
    emit adjustmentsCommitted(current_);
}

void DevelopPanel::onColorTargetEnabledToggled(bool checked) {
    const int idx = currentColorTargetIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.colorTargets.size()))
        return;
    current_.colorTargets[static_cast<size_t>(idx)].enabled = checked;
    scheduleDebounce();
    emit adjustmentsCommitted(current_);
}

void DevelopPanel::syncColorTargetRow(const ColorTargetRow& row) {
    const QSignalBlocker s(row.slider);
    const QSignalBlocker b(row.spin);
    const int idx = currentColorTargetIndex();
    if (idx < 0 || idx >= static_cast<int>(current_.colorTargets.size())) {
        row.slider->setValue(0);
        row.spin->setValue(0);
        return;
    }
    const double v = current_.colorTargets[static_cast<size_t>(idx)].*row.field;
    row.slider->setValue(static_cast<int>(std::lround(v * 100.0)));
    row.spin->setValue(v);
}

void DevelopPanel::syncColorTargetSliders() {
    const int idx = currentColorTargetIndex();
    const bool has = idx >= 0 &&
                     idx < static_cast<int>(current_.colorTargets.size());
    for (const ColorTargetRow& row : colorTargetRows_)
        syncColorTargetRow(row);
    for (const ColorTargetRow& row : colorTargetRows_) {
        row.slider->setEnabled(has);
        row.spin->setEnabled(has);
    }
    colorTargetDeleteBtn_->setEnabled(has);
    {
        const QSignalBlocker b(colorTargetEnabledCheck_);
        colorTargetEnabledCheck_->setChecked(
            has && current_.colorTargets[static_cast<size_t>(idx)].enabled);
        colorTargetEnabledCheck_->setEnabled(has);
    }
    if (has) {
        const lrc::ColorTarget& ct =
            current_.colorTargets[static_cast<size_t>(idx)];
        updateColorTargetSwatch(linearToSwatch(ct.refR, ct.refG, ct.refB));
    } else {
        updateColorTargetSwatch(QColor(40, 40, 40));
    }
}

void DevelopPanel::syncColorTargetSection() {
    const int keep = currentColorTargetIndex();
    {
        const QSignalBlocker b(colorTargetCombo_);
        colorTargetCombo_->clear();
        for (size_t i = 0; i < current_.colorTargets.size(); ++i)
            colorTargetCombo_->addItem(
                tr("点颜色 %1").arg(static_cast<int>(i) + 1));
        // Mirror the mask combo: keep the selection when it is still valid,
        // otherwise explicitly deselect instead of auto-selecting index 0.
        if (keep >= 0 && keep < static_cast<int>(current_.colorTargets.size()))
            colorTargetCombo_->setCurrentIndex(keep);
        else
            colorTargetCombo_->setCurrentIndex(-1);
    }
    syncColorTargetSliders();
}

void DevelopPanel::updateColorTargetSwatch(const QColor& color) {
    colorTargetSwatch_->setStyleSheet(
        QString("background-color: %1; border: 1px solid #666666; "
                "border-radius: 3px;")
            .arg(color.name()));
}

void DevelopPanel::setColorTargetReference(int index, const QColor& displayColor,
                                           float r, float g, float b) {
    if (index < 0) {
        // LR behaviour: sampling without a target creates a fresh one.
        current_.colorTargets.push_back(lrc::ColorTarget{});
        index = static_cast<int>(current_.colorTargets.size()) - 1;
        syncColorTargetSection();
        {
            const QSignalBlocker b(colorTargetCombo_);
            colorTargetCombo_->setCurrentIndex(index);
        }
    }
    if (index < 0 || index >= static_cast<int>(current_.colorTargets.size()))
        return;
    lrc::ColorTarget& ct = current_.colorTargets[static_cast<size_t>(index)];
    ct.refR = r;
    ct.refG = g;
    ct.refB = b;
    ct.enabled = true;
    if (index == currentColorTargetIndex())
        updateColorTargetSwatch(displayColor);
    syncColorTargetSliders();
    scheduleDebounce();
    emit adjustmentsCommitted(current_);
}

void DevelopPanel::setColorTargetSamplerChecked(bool active) {
    const QSignalBlocker b(colorTargetSamplerBtn_);
    colorTargetSamplerBtn_->setChecked(active);
}

void DevelopPanel::onSliderChanged(const Row& row, int value) {
    if (suppressSignals_)
        return;
    const double v = static_cast<double>(value) / 100.0;
    if (!qFuzzyCompare(row.spin->value(), v)) {
        const QSignalBlocker blocker(row.spin);
        row.spin->setValue(v);
    }
    current_.*row.field = v;
    // While Alt is held, show the LR-style diagnostic view instead of the
    // normal preview. Cleared on release (see the sliderReleased connection).
    if (QGuiApplication::keyboardModifiers().testFlag(Qt::AltModifier))
        requestAltVisualization(row);
    else
        scheduleDebounce();
    // QSlider keyboard edits (arrows/PageUp/Home/End) never release the mouse,
    // so persist + push one undo entry here as well. While dragging (dragging_
    // true) the sliderReleased path owns the commit.
    if (!dragging_)
        emit adjustmentsCommitted(current_);
}

void DevelopPanel::onSpinChanged(const Row& row, double value) {
    if (suppressSignals_)
        return;
    const int sliderValue = static_cast<int>(std::lround(value * 100.0));
    if (row.slider->value() != sliderValue) {
        const QSignalBlocker blocker(row.slider);
        row.slider->setValue(sliderValue);
    }
    current_.*row.field = value;
    scheduleDebounce();
}

void DevelopPanel::requestAltVisualization(const Row& row) {
    if (row.vizMode == VizNone)
        return;
    vizActive_ = true;
    emit visualizationRequested(row.vizMode, current_);
}

void DevelopPanel::clearAltVisualization() {
    if (!vizActive_)
        return;
    vizActive_ = false;
    emit visualizationCleared();
}

void DevelopPanel::onHSLSliderChanged(int colorIndex, const HSLRow& row, int value) {
    if (suppressSignals_)
        return;
    const double v = static_cast<double>(value) / 100.0;
    if (!qFuzzyCompare(row.spin->value(), v)) {
        const QSignalBlocker blocker(row.spin);
        row.spin->setValue(v);
    }
    current_.hsl[static_cast<size_t>(colorIndex)].*row.field = v;
    scheduleDebounce();
    // Keyboard edits never release the slider; commit here too (keyboard commit).
    if (!dragging_)
        emit adjustmentsCommitted(current_);
}

void DevelopPanel::onHSLSpinChanged(int colorIndex, const HSLRow& row, double value) {
    if (suppressSignals_)
        return;
    const int sliderValue = static_cast<int>(std::lround(value * 100.0));
    if (row.slider->value() != sliderValue) {
        const QSignalBlocker blocker(row.slider);
        row.slider->setValue(sliderValue);
    }
    current_.hsl[static_cast<size_t>(colorIndex)].*row.field = value;
    scheduleDebounce();
}

void DevelopPanel::onSplitToneSliderChanged(const SplitToneRow& row, int value) {
    if (suppressSignals_)
        return;
    const double v = static_cast<double>(value) / 100.0;
    if (!qFuzzyCompare(row.spin->value(), v)) {
        const QSignalBlocker blocker(row.spin);
        row.spin->setValue(v);
    }
    current_.splitTone.*row.field = v;
    scheduleDebounce();
    // Keyboard edits never release the slider; commit here too (keyboard commit).
    if (!dragging_)
        emit adjustmentsCommitted(current_);
}

void DevelopPanel::onSplitToneSpinChanged(const SplitToneRow& row, double value) {
    if (suppressSignals_)
        return;
    const int sliderValue = static_cast<int>(std::lround(value * 100.0));
    if (row.slider->value() != sliderValue) {
        const QSignalBlocker blocker(row.slider);
        row.slider->setValue(sliderValue);
    }
    current_.splitTone.*row.field = value;
    scheduleDebounce();
}

void DevelopPanel::scheduleDebounce() {
    if (dragging_) {             // dragging: throttle to a coarse preview
        debounce_.stop();
        coarseDebounce_.start();
    } else {                     // pause / release / spin: refine path
        coarseDebounce_.stop();
        debounce_.start();
    }
}

void DevelopPanel::applyCoarseDebounce() {
    emit adjustmentsChangedCoarse(current_);
}

void DevelopPanel::applyDebounce() { emit adjustmentsChanged(current_); }

void DevelopPanel::syncRow(const Row& row) {
    const QSignalBlocker s(row.slider);
    const QSignalBlocker b(row.spin);
    row.slider->setValue(static_cast<int>(std::lround(current_.*row.field * 100.0)));
    row.spin->setValue(current_.*row.field);
}

void DevelopPanel::syncHSLRow(int colorIndex, const HSLRow& row) {
    const QSignalBlocker s(row.slider);
    const QSignalBlocker b(row.spin);
    const double v = current_.hsl[static_cast<size_t>(colorIndex)].*row.field;
    row.slider->setValue(static_cast<int>(std::lround(v * 100.0)));
    row.spin->setValue(v);
}

void DevelopPanel::syncSplitToneRow(const SplitToneRow& row) {
    const QSignalBlocker s(row.slider);
    const QSignalBlocker b(row.spin);
    const double v = current_.splitTone.*row.field;
    row.slider->setValue(static_cast<int>(std::lround(v * 100.0)));
    row.spin->setValue(v);
}

void DevelopPanel::setAdjustments(const lrc::Adjustments& adjustments) {
    suppressSignals_ = true;
    // A pending colour-wheel commit belongs to the previous image's gesture;
    // drop it so switching images never pushes a stale undo entry.
    wheelCommit_.stop();
    // M11: a pending mask-geometry commit belongs to the state being replaced
    // (stroke end / image switch / undo / preset load). Cancelling it here
    // prevents a late flush from emitting adjustmentsCommitted for a stale
    // state -- MainWindow commits the stroke on maskEditFinished, so without
    // this a still-armed timer would double-push an undo entry.
    maskGeometryCommitTimer_.stop();
    maskGeometryDirty_ = false;
    // Loading a new state cancels any in-flight drag gesture; without this a
    // stale dragging_ would suppress the keyboard-commit path below.
    dragging_ = false;
    current_ = adjustments;
    // Clamp out-of-range preset values to each row's range before syncing the
    // widgets, so current_ always equals what the UI displays (the sliders
    // would otherwise clamp silently while current_ kept the raw value).
    for (const Row& row : rows_)
        current_.*row.field = std::clamp(current_.*row.field, row.min, row.max);
    // The style combo is not a slider row; clamp it explicitly so a preset with
    // an out-of-range style still maps onto the enabled Highlight-Priority item.
    current_.vignetteStyle = std::clamp(current_.vignetteStyle, 0, 2);
    for (int ci = 0; ci < static_cast<int>(lrc::ColorRange::Count); ++ci)
        for (const auto& row : hslRows_[static_cast<size_t>(ci)])
            current_.hsl[static_cast<size_t>(ci)].*row.field =
                std::clamp(current_.hsl[static_cast<size_t>(ci)].*row.field,
                           row.min, row.max);
    for (const auto& row : splitToneRows_)
        current_.splitTone.*row.field =
            std::clamp(current_.splitTone.*row.field, row.min, row.max);
    for (auto& mask : current_.masks)
        for (const auto& row : maskRows_)
            mask.*row.field = std::clamp(mask.*row.field, row.min, row.max);
    for (auto& ct : current_.colorTargets)
        for (const auto& row : colorTargetRows_)
            ct.*row.field = std::clamp(ct.*row.field, row.min, row.max);
    for (const Row& row : rows_)
        syncRow(row);
    if (vignetteStyleCombo_) {
        const QSignalBlocker b(vignetteStyleCombo_);
        vignetteStyleCombo_->setCurrentIndex(
            std::clamp(current_.vignetteStyle, 0, 2));
    }
    for (int ci = 0; ci < static_cast<int>(lrc::ColorRange::Count); ++ci)
        for (const auto& row : hslRows_[static_cast<size_t>(ci)])
            syncHSLRow(ci, row);
    for (const auto& row : splitToneRows_)
        syncSplitToneRow(row);
    syncMaskSection();
    syncMaskSliders();
    syncColorTargetSection();
    syncColorTargetSliders();
    curveEditor_->setPoints(adjustments.curve);
    highlightWheel_->setColor(adjustments.splitTone.highlightHue,
                              adjustments.splitTone.highlightSaturation);
    shadowWheel_->setColor(adjustments.splitTone.shadowHue,
                           adjustments.splitTone.shadowSaturation);
    suppressSignals_ = false;
}

void DevelopPanel::setSamplerChecked(bool active) {
    const QSignalBlocker b(samplerButton_);
    samplerButton_->setChecked(active);
}

void DevelopPanel::resetAll() {
    vizActive_ = false;
    setAdjustments(lrc::Adjustments::defaults());
    clearMaskTool();
    curveEditor_->resetToLinear();
    emit adjustmentsChanged(current_);
    emit adjustmentsCommitted(current_);
}

// --- spot removal (M5) ------------------------------------------------------

void DevelopPanel::onSpotModeToggled(lrc::SpotType type, bool checked) {
    if (!checked)
        return;
    spotMode_ = type;
    emit spotModeChanged(type);
}

void DevelopPanel::onSpotRadiusSliderChanged(int value) {
    if (suppressSignals_)
        return;
    const double radius = spotRadiusFromSlider(value);
    if (!qFuzzyCompare(spotRadiusSpin_->value(), radius)) {
        const QSignalBlocker blocker(spotRadiusSpin_);
        spotRadiusSpin_->setValue(radius);
    }
    emit spotRadiusChanged(static_cast<float>(radius));
}

void DevelopPanel::onSpotRadiusSpinChanged(double value) {
    if (suppressSignals_)
        return;
    const double radius = std::clamp(value, kSpotRadiusMin, kSpotRadiusMax);
    const int sliderValue = spotSliderFromRadius(radius);
    if (spotRadiusSlider_->value() != sliderValue) {
        const QSignalBlocker blocker(spotRadiusSlider_);
        spotRadiusSlider_->setValue(sliderValue);
    }
    emit spotRadiusChanged(static_cast<float>(radius));
}

void DevelopPanel::onSpotVisualizeToggled(bool active) {
    if (suppressSignals_)
        return;
    emit visualizeSpotsToggled(active);
}

void DevelopPanel::syncSpotRadius(double radius) {
    if (!spotRadiusSlider_ || !spotRadiusSpin_)
        return;
    const QSignalBlocker b1(spotRadiusSlider_);
    const QSignalBlocker b2(spotRadiusSpin_);
    spotRadiusSlider_->setValue(spotSliderFromRadius(radius));
    spotRadiusSpin_->setValue(std::clamp(radius, kSpotRadiusMin, kSpotRadiusMax));
}

void DevelopPanel::setSpotRadius(float radius) {
    syncSpotRadius(radius);
}

void DevelopPanel::setCurrentSpotCount(int count) {
    if (!spotCountLabel_)
        return;
    spotCountLabel_->setText(tr("污点数: %1").arg(count));
}

// --- metadata (M8) ----------------------------------------------------------

bool DevelopPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == metaCaptionEdit_ && event->type() == QEvent::FocusOut)
        emitMetadataEdited();
    return QWidget::eventFilter(watched, event);
}

void DevelopPanel::setMetadata(const lrc::PhotoMetadata& metadata,
                               const std::vector<std::string>& keywordNames) {
    suppressSignals_ = true;
    // A pending caption debounce belongs to the previous photo; drop it so a
    // late timer cannot push the old photo's text into the new photo.
    metadataCommit_.stop();
    metaTitleEdit_->setText(QString::fromStdString(metadata.title));
    metaCaptionEdit_->setPlainText(QString::fromStdString(metadata.caption));
    metaCopyrightEdit_->setText(QString::fromStdString(metadata.copyright));
    metaCreatorEdit_->setText(QString::fromStdString(metadata.creator));
    keywordNames_ = keywordNames;
    rebuildKeywordChips();
    suppressSignals_ = false;
}

void DevelopPanel::rebuildKeywordChips() {
    if (!keywordChips_ || !keywordChipsLayout_)
        return;
    while (QLayoutItem* item = keywordChipsLayout_->takeAt(0)) {
        if (QWidget* w = item->widget())
            w->deleteLater();
        delete item;
    }
    for (size_t i = 0; i < keywordNames_.size(); ++i) {
        auto* chip = new QPushButton(
            QString::fromStdString(keywordNames_[i]) + tr("  ×"), keywordChips_);
        chip->setProperty("secondary", true);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip(tr("点击移除关键字"));
        const int index = static_cast<int>(i);
        connect(chip, &QPushButton::clicked, this,
                [this, index]() { removeKeywordAt(index); });
        keywordChipsLayout_->addWidget(chip);
    }
    keywordChips_->setVisible(!keywordNames_.empty());
}

void DevelopPanel::addKeyword(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return;
    const std::string name = trimmed.toStdString();
    // Keep the chip list a set: ignore exact duplicates.
    for (const std::string& k : keywordNames_) {
        if (k == name)
            return;
    }
    keywordNames_.push_back(name);
    rebuildKeywordChips();
    emitMetadataEdited();
}

void DevelopPanel::removeKeywordAt(int index) {
    if (index < 0 || index >= static_cast<int>(keywordNames_.size()))
        return;
    keywordNames_.erase(keywordNames_.begin() + index);
    rebuildKeywordChips();
    emitMetadataEdited();
}

void DevelopPanel::scheduleMetadataCommit() {
    if (suppressSignals_)
        return;  // population via setMetadata must not start a commit timer.
    metadataCommit_.start();
}

void DevelopPanel::emitMetadataEdited() {
    metadataCommit_.stop();
    if (suppressSignals_)
        return;
    emit metadataEdited(metaTitleEdit_->text().toStdString(),
                        metaCaptionEdit_->toPlainText().toStdString(),
                        metaCopyrightEdit_->text().toStdString(),
                        metaCreatorEdit_->text().toStdString(),
                        keywordNames_);
}
