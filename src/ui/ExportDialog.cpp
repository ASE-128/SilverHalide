#include "ui/ExportDialog.h"

#include <algorithm>
#include <ctime>

#include "catalog/CatalogDatabase.h"
#include "engine/ExportEngine.h"
#include "utils/RenameTemplate.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVariant>
#include <QVBoxLayout>

namespace {

// Order must match PreviewWorker::ExportFormat.
const char* kFormatNames[] = {
    QT_TR_NOOP("JPEG (*.jpg)"),
    QT_TR_NOOP("PNG (*.png)"),
    QT_TR_NOOP("TIFF 8-bit (*.tif)"),
    QT_TR_NOOP("TIFF 16-bit (*.tif)"),
    QT_TR_NOOP("DNG 线性 (*.dng)"),
};
const char* kFormatSuffixes[] = {".jpg", ".png", ".tif", ".tif", ".dng"};
constexpr int kFormatCount = 5;

// Local "YYYY-MM-DD" for the export {date} token (the export happens today).
std::string todayString() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

} // namespace

ExportDialog::ExportDialog(const QString& suggestedPath, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("导出"));
    setMinimumWidth(420);

    basePath_ = suggestedPath;
    // Strip any existing extension so the format switch can re-append.
    const QFileInfo fi(basePath_);
    basePath_ = fi.path() + "/" + fi.completeBaseName();

    auto* root = new QVBoxLayout(this);
    root->setSpacing(10);

    auto* form = new QFormLayout;
    form->setSpacing(8);

    // Variant / virtual-copy selector. Hidden until MainWindow supplies a
    // variant list via setVariants(); a photo with no virtual copies keeps the
    // legacy single-master behaviour.
    variantCombo_ = new QComboBox(this);
    variantCombo_->setVisible(false);
    form->addRow(tr("副本:"), variantCombo_);

    // Format selector.
    formatCombo_ = new QComboBox(this);
    for (int i = 0; i < kFormatCount; ++i)
        formatCombo_->addItem(tr(kFormatNames[i]));
    form->addRow(tr("格式:"), formatCombo_);

    // Longest-edge size preset. 0 = native resolution; the other
    // entries cap the longest edge of the exported image (area-average
    // downscale, never upscales). ExportEngine::encodeAndWrite applies the
    // cap in linear F32 space before the format-specific encode, so every
    // format takes the same path.
    sizeCombo_ = new QComboBox(this);
    sizeCombo_->addItem(tr("原始尺寸"), QVariant(0));
    sizeCombo_->addItem(tr("1024 px"), QVariant(1024));
    sizeCombo_->addItem(tr("1920 px"), QVariant(1920));
    sizeCombo_->addItem(tr("2048 px"), QVariant(2048));
    sizeCombo_->addItem(tr("4096 px"), QVariant(4096));
    sizeCombo_->setCurrentIndex(0);
    form->addRow(tr("尺寸:"), sizeCombo_);

    // Output path + browse.
    auto* pathRow = new QHBoxLayout;
    pathRow->setSpacing(6);
    pathEdit_ = new QLineEdit(this);
    pathRow->addWidget(pathEdit_, 1);
    auto* browseBtn = new QPushButton(tr("浏览…"), this);
    browseBtn->setProperty("secondary", true);
    pathRow->addWidget(browseBtn);
    form->addRow(tr("输出:"), pathRow);

    // JPEG quality (hidden for lossless formats).
    qualityRow_ = new QWidget(this);
    auto* qualityLayout = new QHBoxLayout(qualityRow_);
    qualityLayout->setContentsMargins(0, 0, 0, 0);
    qualityLayout->setSpacing(6);
    qualitySlider_ = new QSlider(Qt::Horizontal, qualityRow_);
    qualitySlider_->setRange(0, 100);
    qualitySlider_->setValue(92);
    qualityLayout->addWidget(qualitySlider_, 1);
    qualityValueLabel_ = new QLabel(tr("92"), qualityRow_);
    qualityValueLabel_->setFixedWidth(24);
    qualityLayout->addWidget(qualityValueLabel_);
    form->addRow(tr("JPEG 质量:"), qualityRow_);

    // ICC profile to embed in the exported file (0 = none / byte-identical legacy
    // output). Order must match ExportEngine::ExportSpec::IccChoice.
    iccCombo_ = new QComboBox(this);
    iccCombo_->addItem(tr("无 (sRGB 默认)"), QVariant(0));
    iccCombo_->addItem(tr("sRGB"), QVariant(1));
    iccCombo_->addItem(tr("Adobe RGB"), QVariant(2));
    iccCombo_->addItem(tr("Display P3"), QVariant(3));
    iccCombo_->setCurrentIndex(0);
    form->addRow(tr("ICC:"), iccCombo_);

    // Watermark overlay (PNG with alpha). Hidden until an overlay is chosen.
    auto* wmRow = new QWidget(this);
    auto* wmLayout = new QFormLayout(wmRow);
    wmLayout->setContentsMargins(0, 0, 0, 0);
    wmLayout->setSpacing(6);
    wmEnable_ = new QCheckBox(tr("启用水印"), wmRow);
    wmLayout->addRow(wmEnable_);

    auto* wmPathRow = new QHBoxLayout;
    wmPathRow->setSpacing(6);
    wmPathLabel_ = new QLabel(tr("未选择"), wmRow);
    wmPathRow->addWidget(wmPathLabel_, 1);
    auto* wmBrowse = new QPushButton(tr("选择 PNG…"), wmRow);
    wmBrowse->setProperty("secondary", true);
    wmPathRow->addWidget(wmBrowse);
    wmLayout->addRow(tr("图片:"), wmPathRow);

    wmPosition_ = new QComboBox(wmRow);
    wmPosition_->addItem(tr("左上"), QVariant(0));
    wmPosition_->addItem(tr("右上"), QVariant(1));
    wmPosition_->addItem(tr("左下"), QVariant(2));
    wmPosition_->addItem(tr("右下"), QVariant(3));
    wmPosition_->addItem(tr("居中"), QVariant(4));
    wmPosition_->setCurrentIndex(4);
    wmLayout->addRow(tr("位置:"), wmPosition_);

    wmOpacity_ = new QSlider(Qt::Horizontal, wmRow);
    wmOpacity_->setRange(1, 100);
    wmOpacity_->setValue(100);
    wmLayout->addRow(tr("不透明度:"), wmOpacity_);

    wmScale_ = new QSlider(Qt::Horizontal, wmRow);
    wmScale_->setRange(1, 50);
    wmScale_->setValue(20);  // ~0.20 of the output's long edge
    wmLayout->addRow(tr("大小:"), wmScale_);

    wmRow_ = wmRow;
    wmRow_->setVisible(false);
    form->addRow(tr("水印:"), wmRow_);

    connect(wmBrowse, &QPushButton::clicked, this, &ExportDialog::browseWatermark);
    connect(wmEnable_, &QCheckBox::toggled, this, &ExportDialog::onWatermarkToggle);

    // Batch rename on export. When enabled, MainWindow exports the master + every
    // virtual copy into the chosen folder, naming outputs from the template
    // ({date}/{index}/{base}); the live preview mirrors what the batch will
    // produce.
    renameBox_ = new QCheckBox(tr("批量导出时重命名文件"), this);
    form->addRow(renameBox_);
    renameEdit_ = new QLineEdit(this);
    renameEdit_->setPlaceholderText(tr("{date}_{base}_{index:2}"));
    form->addRow(tr("模板:"), renameEdit_);
    renamePreview_ = new QLabel(this);
    renamePreview_->setProperty("caption", true);
    renamePreview_->setWordWrap(true);
    renamePreview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(renamePreview_);
    refreshRenamePreview();

    root->addLayout(form);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("导出"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    root->addWidget(buttons);

    connect(formatCombo_, &QComboBox::currentIndexChanged, this,
            &ExportDialog::onFormatChanged);
    connect(renameBox_, &QCheckBox::toggled, this,
            &ExportDialog::refreshRenamePreview);
    connect(renameEdit_, &QLineEdit::textChanged, this,
            &ExportDialog::refreshRenamePreview);
    connect(variantCombo_,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { emit variantChosen(selectedVariantId()); });
    connect(browseBtn, &QPushButton::clicked, this, &ExportDialog::browse);
    connect(pathEdit_, &QLineEdit::textEdited, this,
            [this]() { pathUserEdited_ = true; });
    connect(buttons, &QDialogButtonBox::accepted, this,
            &ExportDialog::validateAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(qualitySlider_, &QSlider::valueChanged, this,
            [this](int v) { qualityValueLabel_->setText(QString::number(v)); });

    onFormatChanged(0);
}

QString ExportDialog::outputPath() const { return pathEdit_->text().trimmed(); }

int ExportDialog::formatIndex() const { return formatCombo_->currentIndex(); }

int ExportDialog::jpegQuality() const { return qualitySlider_->value(); }

int ExportDialog::maxDim() const {
    // 0 (the first entry, "原始尺寸") leaves the export at native resolution;
    // any other preset caps the longest edge.
    bool ok = false;
    const int v = sizeCombo_ ? sizeCombo_->currentData().toInt(&ok) : 0;
    return ok ? v : 0;
}

void ExportDialog::setVariants(const std::vector<lrc::EditVariant>& variants) {
    // Cache the batch targets (master + copies) so the rename preview can render
    // one row per output without a second catalog query.
    variants_ = variants;

    // Block signals while repopulating so variantChosen() reflects only real
    // user picks, not the initial fill.
    const QSignalBlocker blocker(variantCombo_);
    variantCombo_->clear();

    bool hasCopy = false;
    int masterIndex = -1;
    for (std::size_t i = 0; i < variants.size(); ++i) {
        const lrc::EditVariant& v = variants[i];
        if (!v.isMaster)
            hasCopy = true;
        QString label;
        if (v.isMaster) {
            label = tr("母版");
            masterIndex = static_cast<int>(i);
        } else if (!v.name.empty()) {
            label = QString::fromUtf8(v.name.data(),
                                      static_cast<int>(v.name.size()));
        } else {
            label = tr("副本 %1").arg(static_cast<qlonglong>(v.variantId));
        }
        variantCombo_->addItem(label,
                               QVariant::fromValue<qlonglong>(v.variantId));
    }

    // Show the selector only when there is at least one virtual copy to pick.
    // A photo with no copies (or only the master row) keeps the legacy layout.
    variantsAvailable_ = hasCopy;
    variantCombo_->setVisible(hasCopy);
    if (masterIndex >= 0)
        variantCombo_->setCurrentIndex(masterIndex);
    else if (variantCombo_->count() > 0)
        variantCombo_->setCurrentIndex(0);

    refreshRenamePreview();
}

bool ExportDialog::hasVariants() const { return variantsAvailable_; }

qint64 ExportDialog::selectedVariantId() const {
    if (!variantsAvailable_)
        return 0;
    bool ok = false;
    const qlonglong v = variantCombo_->currentData().toLongLong(&ok);
    return ok ? static_cast<qint64>(v) : 0;
}

QString ExportDialog::defaultFileName(const QString& baseName, int formatIndex) {
    if (formatIndex < 0 || formatIndex >= kFormatCount)
        formatIndex = 0;
    return baseName + QString::fromLatin1(kFormatSuffixes[formatIndex]);
}

void ExportDialog::browse() {
    const int idx = formatIndex();
    const QString filter = QString::fromLatin1("图像 (*%1)").arg(
        QString::fromLatin1(kFormatSuffixes[idx]));
    const QString chosen = QFileDialog::getSaveFileName(
        this, tr("选择导出路径"), pathEdit_->text(), filter);
    if (!chosen.isEmpty())
        pathEdit_->setText(chosen);
}

void ExportDialog::onFormatChanged(int index) {
    if (index < 0 || index >= kFormatCount)
        return;
    // Show the quality row only for the lossy JPEG format.
    qualityRow_->setVisible(index == 0);

    // Keep the dialog honest about what each format actually keeps.
    //   IC  profile  : embedded for JPEG/PNG/TIFF (pixels are converted to match, see
    //                  Engine sRGBEncodedToProfile). DNG stores linear ProPhoto data
    //                  that is not converted into any display space, so embedding a
    //                  display ICC would mislabel it -> disable the choice.
    //   watermark    : composited onto the display (U8) rasters JPEG/PNG/TIFF-8 only;
    //                  TIFF-16 and DNG are linear/archival and never carry it -> disable.
    const bool supportsIcc = (index != 4);                            // all but DNG
    const bool supportsWatermark = (index == 0 || index == 1 || index == 2); // JPEG/PNG/TIFF8
    if (iccCombo_)
        iccCombo_->setEnabled(supportsIcc);
    if (wmRow_)
        wmRow_->setEnabled(supportsWatermark);

    currentSuffix_ = QString::fromLatin1(kFormatSuffixes[index]);

    // Keep the user's typed base name; switching formats only swaps the file
    // extension. If the user deliberately cleared the field, leave it empty
    // instead of refilling the default name.
    const QString text = pathEdit_->text().trimmed();
    if (!text.isEmpty()) {
        const QFileInfo fi(text);
        basePath_ = fi.path() + "/" + fi.completeBaseName();
        pathEdit_->setText(basePath_ + currentSuffix_);
    } else if (!pathUserEdited_) {
        // Path was never manually edited (e.g. the constructor default) —
        // apply it now.
        pathEdit_->setText(basePath_ + currentSuffix_);
    }
}

void ExportDialog::validateAndAccept() {
    if (pathEdit_->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("导出路径为空"),
                             tr("请先选择或输入导出路径。"));
        pathEdit_->setFocus();
        return;
    }
    accept();
}

int ExportDialog::iccIndex() const {
    bool ok = false;
    const int v = iccCombo_ ? iccCombo_->currentData().toInt(&ok) : 0;
    // Clamp to the valid IccChoice range (0..3). Unknown/absent -> none.
    if (ok && v >= 0 && v <= 3)
        return v;
    return 0;
}

ExportWatermark ExportDialog::watermark() const {
    ExportWatermark wm;
    wm.enabled = wmEnable_ && wmEnable_->isChecked() && !wmOverlay_.isNull();
    if (!wm.enabled)
        return wm;
    wm.overlayW = wmOverlay_.width();
    wm.overlayH = wmOverlay_.height();
    wm.position = wmPosition_ ? wmPosition_->currentData().toInt() : 4;
    wm.opacity = (wmOpacity_ ? wmOpacity_->value() : 100) / 100.0f;
    wm.scale = (wmScale_ ? wmScale_->value() : 20) / 100.0f;
    // Convert the QImage to straight-alpha ARGB32 and repack as RGBA bytes that
    // mirror the engine's straight-alpha RGBA overlay format.
    QImage rgba = wmOverlay_.convertToFormat(QImage::Format_ARGB32);
    wm.rgba.reserve(wmOverlay_.size().width() * wmOverlay_.size().height());
    for (int y = 0; y < wmOverlay_.height(); ++y) {
        const QRgb* line =
            reinterpret_cast<const QRgb*>(rgba.constScanLine(y));
        for (int x = 0; x < wmOverlay_.width(); ++x) {
            const QRgb px = line[x];
            wm.rgba.push_back(static_cast<uint8_t>(qRed(px)));
            wm.rgba.push_back(static_cast<uint8_t>(qGreen(px)));
            wm.rgba.push_back(static_cast<uint8_t>(qBlue(px)));
            wm.rgba.push_back(static_cast<uint8_t>(qAlpha(px)));
        }
    }
    return wm;
}

void ExportDialog::browseWatermark() {
    const QString chosen = QFileDialog::getOpenFileName(
        this, tr("选择水印图片"), QString(),
        tr("图片 (*.png *.jpg *.jpeg *.bmp)"));
    if (chosen.isEmpty())
        return;
    QImage img(chosen);
    if (img.isNull()) {
        QMessageBox::warning(this, tr("无法读取图片"),
                             tr("无法解码所选图片文件: %1").arg(chosen));
        return;
    }
    wmOverlay_ = img;
    wmPathLabel_->setText(QFileInfo(chosen).fileName());
    wmRow_->setVisible(true);
    wmEnable_->setChecked(true);  // picking a file implies the user wants it on
}

void ExportDialog::onWatermarkToggle(bool on) {
    if (!on)
        return;  // keep the overlay in memory so re-checking is instant
    if (wmOverlay_.isNull())
        wmRow_->setVisible(false);
}

bool ExportDialog::renameEnabled() const {
    return renameBox_->isChecked() && !renameTemplate().isEmpty();
}

QString ExportDialog::renameTemplate() const {
    return renameEdit_->text().trimmed();
}

void ExportDialog::refreshRenamePreview() {
    const bool on = renameBox_->isChecked();
    renameEdit_->setEnabled(on);
    renamePreview_->setVisible(on);

    if (!on || renameTemplate().isEmpty()) {
        renamePreview_->setText(on ? tr("启用后此处预览批量导出文件名。") : QString());
        return;
    }

    // Batch = the master plus every virtual copy, ordered by the catalog. index
    // is 1-based, {base} is the photo's base name and {date} is today's date —
    // the same inputs MainWindow uses when it builds the real batch.
    const std::string tmpl = renameTemplate().toStdString();
    const std::string base =
        QFileInfo(basePath_).completeBaseName().toStdString();
    const std::string date = todayString();
    const std::string suffix = kFormatSuffixes[formatIndex()];

    const int total = static_cast<int>(variants_.size());
    const int shown = std::min(static_cast<int>(total), static_cast<int>(5));
    QStringList preview;
    for (int i = 0; i < shown; ++i) {
        const lrc::EditVariant& v = variants_[i];
        QString label;
        if (v.isMaster)
            label = tr("母版");
        else if (!v.name.empty())
            label = QString::fromUtf8(v.name.data(),
                                      static_cast<int>(v.name.size()));
        else
            label = tr("副本 %1").arg(static_cast<qlonglong>(v.variantId));

        lrc::RenameTemplateArgs a;
        a.tmpl = tmpl;
        a.base = base;
        a.index = i + 1;
        a.indexWidth = 2;
        a.date = date;
        preview << label + " → " +
                       QString::fromStdString(renderRenameTemplate(a) + suffix);
    }
    if (total > shown)
        preview << tr("… 共 %1 个").arg(total);
    renamePreview_->setText(preview.join(QStringLiteral("\n")));
}
