#include "ui/ExportDialog.h"

#include "catalog/CatalogDatabase.h"

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

    // Variant / virtual-copy selector (M6). Hidden until MainWindow supplies a
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

    root->addLayout(form);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("导出"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    root->addWidget(buttons);

    connect(formatCombo_, &QComboBox::currentIndexChanged, this,
            &ExportDialog::onFormatChanged);
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

void ExportDialog::setVariants(const std::vector<lrc::EditVariant>& variants) {
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
