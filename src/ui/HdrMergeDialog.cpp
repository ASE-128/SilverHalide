#include "ui/HdrMergeDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace {
constexpr int kPathRole = Qt::UserRole;
} // namespace

HdrMergeDialog::HdrMergeDialog(const QVector<HdrMergeImage>& images,
                               QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("HDR 合成"));
    setMinimumSize(460, 420);

    auto* root = new QVBoxLayout(this);
    root->setSpacing(8);

    auto* hint = new QLabel(
        tr("勾选 2 张或更多同一场景、不同曝光的照片，然后点击「合并」。"),
        this);
    hint->setWordWrap(true);
    root->addWidget(hint);

    list_ = new QListWidget(this);
    for (const HdrMergeImage& img : images) {
        auto* item = new QListWidgetItem(img.fileName, list_);
        item->setToolTip(img.filePath);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        item->setData(kPathRole, img.filePath);
    }
    list_->setSelectionMode(QAbstractItemView::NoSelection);
    root->addWidget(list_, 1);

    statusLabel_ = new QLabel(tr("已选择 0 张"), this);
    root->addWidget(statusLabel_);

    deghostCheck_ = new QCheckBox(tr("启用去鬼影（适合手持/运动场景）"), this);
    root->addWidget(deghostCheck_);

    auto* pathRow = new QHBoxLayout;
    pathRow->setSpacing(6);
    pathEdit_ = new QLineEdit(this);
    QString defaultName = QStringLiteral("merged_hdr.dng");
    if (!images.isEmpty()) {
        const QFileInfo fi(images.first().filePath);
        if (!fi.completeBaseName().isEmpty())
            defaultName = fi.completeBaseName() + QStringLiteral("_hdr.dng");
    }
    pathEdit_->setText(QStandardPaths::writableLocation(
                           QStandardPaths::DocumentsLocation) +
                       QLatin1Char('/') + defaultName);
    pathRow->addWidget(pathEdit_, 1);
    auto* browseBtn = new QPushButton(tr("浏览…"), this);
    browseBtn->setProperty("secondary", true);
    pathRow->addWidget(browseBtn);
    root->addLayout(pathRow);

    buttons_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons_->button(QDialogButtonBox::Ok)->setText(tr("合并"));
    buttons_->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(false);
    root->addWidget(buttons_);

    connect(browseBtn, &QPushButton::clicked, this, &HdrMergeDialog::browse);
    connect(list_, &QListWidget::itemChanged, this,
            &HdrMergeDialog::onSelectionChanged);
    connect(buttons_, &QDialogButtonBox::accepted, this,
            &HdrMergeDialog::validateAndAccept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QStringList HdrMergeDialog::selectedPaths() const {
    QStringList out;
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem* item = list_->item(i);
        if (item->checkState() == Qt::Checked)
            out << item->data(kPathRole).toString();
    }
    return out;
}

bool HdrMergeDialog::deghost() const { return deghostCheck_->isChecked(); }

QString HdrMergeDialog::outputPath() const {
    return pathEdit_->text().trimmed();
}

void HdrMergeDialog::browse() {
    const QString chosen = QFileDialog::getSaveFileName(
        this, tr("选择输出 DNG"), pathEdit_->text(), tr("DNG (*.dng)"));
    if (!chosen.isEmpty())
        pathEdit_->setText(chosen);
}

void HdrMergeDialog::onSelectionChanged() {
    const int count = selectedPaths().size();
    statusLabel_->setText(tr("已选择 %1 张").arg(count));
    if (buttons_)
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(count >= 2);
}

void HdrMergeDialog::validateAndAccept() {
    QString out = pathEdit_->text().trimmed();
    if (out.isEmpty()) {
        QMessageBox::warning(this, tr("输出路径为空"),
                             tr("请先选择或输入 HDR 输出路径。"));
        pathEdit_->setFocus();
        return;
    }
    // The merge always writes a DNG, so the output name must carry a .dng
    // suffix; otherwise the file would be mislabeled with the wrong extension.
    if (!out.endsWith(QStringLiteral(".dng"), Qt::CaseInsensitive)) {
        out += QStringLiteral(".dng");
        pathEdit_->setText(out);
    }
    accept();
}
