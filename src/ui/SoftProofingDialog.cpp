#include "ui/SoftProofingDialog.h"

#include <algorithm>
#include <array>

#include <QColorDialog>
#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "engine/ColorManager.h"

namespace lrc {

namespace {

QString intentLabel(RenderingIntent intent) {
    switch (intent) {
        case RenderingIntent::Perceptual:          return SoftProofingDialog::tr("感知");
        case RenderingIntent::RelativeColorimetric:return SoftProofingDialog::tr("相对比色");
        case RenderingIntent::AbsoluteColorimetric:return SoftProofingDialog::tr("绝对比色");
        case RenderingIntent::Saturation:          return SoftProofingDialog::tr("饱和度");
    }
    return {};
}

} // namespace

SoftProofingDialog::SoftProofingDialog(ColorManager* cm, QWidget* parent)
    : QDialog(parent), cm_(cm) {
    setWindowTitle(tr("软打样"));
    resize(520, 440);

    auto* root = new QVBoxLayout(this);

    enableCheck_ = new QCheckBox(tr("在预览上启用软打样"), this);
    enableCheck_->setChecked(false);
    root->addWidget(enableCheck_);

    auto* formBox = new QGroupBox(tr("打样参数"), this);
    auto* form = new QFormLayout(formBox);
    profileCombo_ = new QComboBox(this);
    populateProfileCombo();
    auto* loadProfileBtn = new QPushButton(tr("从文件加载…"), this);
    auto* profileRow = new QHBoxLayout();
    profileRow->addWidget(profileCombo_, 1);
    profileRow->addWidget(loadProfileBtn);
    auto* profileWrap = new QWidget(this);
    profileWrap->setLayout(profileRow);
    form->addRow(tr("目标配置文件："), profileWrap);

    intentCombo_ = new QComboBox(this);
    populateIntentCombo();
    form->addRow(tr("渲染意图："), intentCombo_);

    bpcCheck_ = new QCheckBox(tr("使用黑场补偿（BPC）"), this);
    bpcCheck_->setChecked(true);
    paperCheck_ = new QCheckBox(tr("模拟纸张与油墨（绝对意图）"), this);
    form->addRow(QString(), bpcCheck_);
    form->addRow(QString(), paperCheck_);
    root->addWidget(formBox);

    auto* gamutBox = new QGroupBox(tr("色域警告"), this);
    auto* gamutLayout = new QFormLayout(gamutBox);
    highlightCheck_ = new QCheckBox(tr("高亮超出色域的像素"), this);
    highlightCheck_->setChecked(false);
    highlightColorBtn_ = new QPushButton(tr("选择颜色…"), this);
    highlightColorBtn_->setStyleSheet(
        QString("background: %1; color: white;").arg(highlightColor_.name()));
    auto* highlightRow = new QHBoxLayout();
    highlightRow->addWidget(highlightCheck_);
    highlightRow->addWidget(highlightColorBtn_);
    auto* highlightWrap = new QWidget(this);
    highlightWrap->setLayout(highlightRow);
    gamutLayout->addRow(highlightWrap);
    previewLabel_ = new QLabel(
        tr("提示：调整参数后，预览会实时重新渲染。"), this);
    previewLabel_->setWordWrap(true);
    gamutLayout->addRow(previewLabel_);
    root->addWidget(gamutBox);

    auto* btnRow = new QHBoxLayout();
    exportBtn_ = new QPushButton(tr("导出色域报告…"), this);
    closeBtn_ = new QPushButton(tr("关闭"), this);
    btnRow->addWidget(exportBtn_);
    btnRow->addStretch(1);
    btnRow->addWidget(closeBtn_);
    root->addLayout(btnRow);

    connect(enableCheck_, &QCheckBox::toggled, this, &SoftProofingDialog::emitChanged);
    connect(profileCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &SoftProofingDialog::emitChanged);
    connect(intentCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &SoftProofingDialog::emitChanged);
    connect(bpcCheck_, &QCheckBox::toggled, this, &SoftProofingDialog::emitChanged);
    connect(paperCheck_, &QCheckBox::toggled, this, &SoftProofingDialog::emitChanged);
    connect(highlightCheck_, &QCheckBox::toggled, this, &SoftProofingDialog::emitChanged);
    connect(highlightColorBtn_, &QPushButton::clicked,
            this, &SoftProofingDialog::onHighlightColor);
    connect(loadProfileBtn, &QPushButton::clicked,
            this, &SoftProofingDialog::onLoadProfileFromFile);
    connect(exportBtn_, &QPushButton::clicked,
            this, &SoftProofingDialog::onExportReport);
    connect(closeBtn_, &QPushButton::clicked, this, &QDialog::accept);

    // Default to Perceptual (the most common proofing intent for photographic
    // output).  RelativeColorimetric is the second-most common and can be
    // selected by the user; ColorManager doesn't expose a user-configurable
    // default yet, so we anchor to enum index 0 (Perceptual) which matches
    // populateIntentCombo()'s insertion order.
    (void)cm_;
    intentCombo_->setCurrentIndex(0);
}

SoftProofParams SoftProofingDialog::currentParams() const {
    SoftProofParams p;
    p.destinationProfilePath = customProfilePath_.isEmpty()
        ? profileCombo_->currentData(Qt::UserRole + 1).toString().toStdString()
        : customProfilePath_.toStdString();
    p.intent = static_cast<RenderingIntent>(
        intentCombo_->currentData().toInt());
    p.useBlackPointCompensation = bpcCheck_->isChecked();
    p.simulatePaperWhite = paperCheck_->isChecked();
    p.showOutOfGamut = highlightCheck_->isChecked();
    p.outOfGamutR = highlightColor_.redF();
    p.outOfGamutG = highlightColor_.greenF();
    p.outOfGamutB = highlightColor_.blueF();
    return p;
}

bool SoftProofingDialog::proofingEnabled() const {
    return enableCheck_->isChecked();
}

void SoftProofingDialog::emitChanged() {
    emit paramsChanged(currentParams(), proofingEnabled());
}

void SoftProofingDialog::onLoadProfileFromFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("加载 ICC 配置文件"), QString(),
        tr("ICC 配置文件 (*.icc *.icm);;所有文件 (*)"));
    if (path.isEmpty()) return;
    // Verify it decodes before committing.
    SoftProofing sp;
    if (!sp.loadProofProfile(path.toStdString())) {
        QMessageBox::warning(this, tr("软打样"),
                             tr("文件\n%1\n不是有效的 ICC 配置文件。")
                                 .arg(path));
        return;
    }
    customProfilePath_ = path;
    // Append a synthetic entry so the dropdown reflects the active source.
    const QString label = tr("自定义：%1").arg(path);
    // Replace any prior custom entry (we use -1 as a sentinel) to avoid
    // endlessly appending new rows every time the user re-picks a file.
    for (int i = 0; i < profileCombo_->count(); ++i) {
        if (profileCombo_->itemData(i).toInt() == -1) {
            profileCombo_->setItemText(i, label);
            profileCombo_->setCurrentIndex(i);
            emitChanged();
            return;
        }
    }
    profileCombo_->insertItem(0, label, -1);
    profileCombo_->setCurrentIndex(0);
    emitChanged();
}

void SoftProofingDialog::onHighlightColor() {
    const QColor chosen = QColorDialog::getColor(highlightColor_, this,
                                                 tr("超出色域的高亮"));
    if (!chosen.isValid()) return;
    highlightColor_ = chosen;
    highlightColorBtn_->setStyleSheet(
        QString("background: %1; color: white;").arg(highlightColor_.name()));
    emitChanged();
}

void SoftProofingDialog::onExportReport() {
    emit exportGamutReportRequested();
}

void SoftProofingDialog::populateIntentCombo() {
    const std::array<RenderingIntent, 4> all = {
        RenderingIntent::Perceptual,
        RenderingIntent::RelativeColorimetric,
        RenderingIntent::AbsoluteColorimetric,
        RenderingIntent::Saturation,
    };
    for (RenderingIntent i : all)
        intentCombo_->addItem(intentLabel(i), static_cast<int>(i));
}

void SoftProofingDialog::populateProfileCombo() {
    // Built-in candidates that the ColorManager already knows about;
    // additional installed profiles can easily be added by extending
    // ColorManager::listInstalledProfiles when a real profile search is
    // needed.  For now we expose the canonical ones.
    const auto add = [&](const QString& label, const std::string& key) {
        // UserRole -> 0 for built-ins, -1 later for custom.
        // UserRole+1 -> path or ColorManager key.
        profileCombo_->addItem(label, 0);
        profileCombo_->setItemData(profileCombo_->count() - 1,
                                   QString::fromStdString(key),
                                   Qt::UserRole + 1);
    };
    add(tr("sRGB IEC61966-2.1（默认）"), "srgb");
    add(tr("Display P3"), "display_p3");
    add(tr("Adobe RGB (1998)"), "adobe_rgb");
    add(tr("ProPhoto RGB (ROMM RGB)"), "prophoto");
    add(tr("SWOP Coated GRACoL 2006 (CMYK proof)"), "swop_coated_gracol");
    add(tr("Fogra39 (ISO Coated v2 CMYK)"), "fogra39");
    // Select sRGB as the default proof target (most common case for web
    // preview) — users who print should pick the paper profile instead.
    profileCombo_->setCurrentIndex(0);
}

} // namespace lrc
