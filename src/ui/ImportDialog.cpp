#include "ui/ImportDialog.h"

#include <algorithm>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

#include "catalog/FolderMonitor.h"
#include "utils/RenameTemplate.h"

ImportDialog::ImportDialog(QWidget* parent)
    : ImportDialog(QString(), parent) {}

ImportDialog::ImportDialog(const QString& defaultDestDir, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("导入照片"));
    setMinimumWidth(460);

    auto* root = new QVBoxLayout(this);
    root->setSpacing(10);

    // Pick photos (RAW/DNG/JPEG/PNG/TIFF), with a running count selected.
    auto* pickRow = new QHBoxLayout;
    pickRow->setSpacing(6);
    browseButton_ = new QPushButton(tr("选择文件…"), this);
    browseButton_->setProperty("secondary", true);
    pickRow->addWidget(browseButton_);
    summaryLabel_ = new QLabel(tr("未选择文件"), this);
    summaryLabel_->setProperty("caption", true);
    pickRow->addWidget(summaryLabel_, 1);
    root->addLayout(pickRow);

    // Lightroom-style import policy: Add (reference in place, no move), Copy
    // (copy into the managed library, keep the source), Move (copy then remove
    // the source). "Copy to folder" is the default.
    addRadio_ = new QRadioButton(tr("添加"), this);
    addRadio_->setToolTip(tr("不移动文件，仅编入目录"));
    copyRadio_ = new QRadioButton(tr("复制到目录"), this);
    copyRadio_->setToolTip(tr("复制文件到目标目录，保留原始文件"));
    moveRadio_ = new QRadioButton(tr("移动"), this);
    moveRadio_->setToolTip(tr("移动文件到目标目录，删除原始文件"));
    copyRadio_->setChecked(true); // default
    root->addWidget(addRadio_);
    root->addWidget(copyRadio_);
    root->addWidget(moveRadio_);

    // Destination folder; disabled in Add mode (nothing is moved/copied).
    auto* destRow = new QHBoxLayout;
    destRow->setSpacing(6);
    destEdit_ = new QLineEdit(defaultDestDir, this);
    destEdit_->setPlaceholderText(tr("目标目录"));
    destRow->addWidget(destEdit_, 1);
    browseDestButton_ = new QPushButton(tr("浏览…"), this);
    browseDestButton_->setProperty("secondary", true);
    destRow->addWidget(browseDestButton_);
    root->addLayout(destRow);

    // Lightroom's "Import as DNG": convert RAW before cataloging.
    convertBox_ = new QCheckBox(tr("导入时转换为 DNG"), this);
    root->addWidget(convertBox_);
    auto* hint = new QLabel(
        tr("仅 RAW 会先转为 .dng 再导入目录；「添加/复制」模式下原始文件保留，"
           "「移动」模式下转换完成后原始文件会被删除。JPEG/PNG/TIFF 保持原样。"), this);
    hint->setWordWrap(true);
    hint->setEnabled(false); // dimmed hint under the checkbox
    root->addWidget(hint);

    // Lightroom-style rename on import. The user types a template using {date},
    // {index[:W]} and {base}; the library renames each file to the rendered name
    // (collisions get a numeric suffix) before cataloging it.
    renameBox_ = new QCheckBox(tr("导入时重命名文件"), this);
    root->addWidget(renameBox_);
    auto* renameRow = new QHBoxLayout;
    renameRow->setSpacing(6);
    renameEdit_ = new QLineEdit(this);
    renameEdit_->setPlaceholderText(tr("{date}_{base}_{index:4}"));
    renameRow->addWidget(renameEdit_, 1);
    root->addLayout(renameRow);
    renamePreview_ = new QLabel(this);
    renamePreview_->setProperty("caption", true);
    renamePreview_->setWordWrap(true);
    renamePreview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(renamePreview_);
    refreshRenamePreview();

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("导入"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    root->addWidget(buttons);

    connect(browseButton_, &QPushButton::clicked, this, &ImportDialog::browse);
    connect(browseDestButton_, &QPushButton::clicked, this,
            &ImportDialog::browseDest);
    connect(addRadio_, &QRadioButton::toggled, this,
            &ImportDialog::refreshMode);
    connect(renameBox_, &QCheckBox::toggled, this,
            &ImportDialog::refreshRenamePreview);
    connect(renameEdit_, &QLineEdit::textChanged, this,
            &ImportDialog::refreshRenamePreview);
    connect(buttons, &QDialogButtonBox::accepted, this,
            &ImportDialog::validateAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    refreshMode();
    refreshSummary();
}

QStringList ImportDialog::selectedPaths() const { return paths_; }

bool ImportDialog::convertToDng() const { return convertBox_->isChecked(); }

lrc::ImportMode ImportDialog::importMode() const {
    if (moveRadio_->isChecked())
        return lrc::ImportMode::Move;
    if (addRadio_->isChecked())
        return lrc::ImportMode::Add;
    return lrc::ImportMode::Copy;
}

QString ImportDialog::destinationDir() const {
    return destEdit_->text().trimmed();
}

void ImportDialog::refreshMode() {
    const bool needsDest = !addRadio_->isChecked();
    destEdit_->setEnabled(needsDest);
    browseDestButton_->setEnabled(needsDest);
}

void ImportDialog::browse() {
    // The file filter mirrors the extension list the folder monitor accepts
    // (RAW/DNG plus the app's own export formats JPEG/PNG/TIFF). Each entry
    // must be a glob pattern (*.ext) or the native dialog matches nothing.
    QStringList patterns;
    for (const std::string& e : lrc::FolderMonitor::importExtensions())
        patterns << QStringLiteral("*") + QString::fromStdString(e);
    const QString filter =
        tr("图像 (%1);;所有文件 (*)").arg(patterns.join(QStringLiteral(" ")));
    const QStringList chosen = QFileDialog::getOpenFileNames(
        this, tr("选择照片"), QDir::homePath(), filter);
    if (!chosen.isEmpty()) {
        paths_ = chosen;
        refreshSummary();
    }
}

void ImportDialog::browseDest() {
    QString start = destEdit_->text();
    if (start.isEmpty())
        start = QDir::homePath();
    const QString dir =
        QFileDialog::getExistingDirectory(this, tr("选择目标目录"), start);
    if (!dir.isEmpty())
        destEdit_->setText(QDir::toNativeSeparators(dir));
}

void ImportDialog::refreshSummary() {
    if (paths_.isEmpty()) {
        summaryLabel_->setText(tr("未选择文件"));
    } else {
        summaryLabel_->setText(tr("已选择 %1 个文件").arg(paths_.size()));
    }
}

void ImportDialog::validateAndAccept() {
    if (paths_.isEmpty()) {
        QMessageBox::warning(this, tr("未选择文件"),
                             tr("请先选择要导入的照片文件。"));
        browseButton_->setFocus();
        return;
    }
    accept();
}

bool ImportDialog::renameEnabled() const {
    return renameBox_->isChecked() && !renameTemplate().isEmpty();
}

QString ImportDialog::renameTemplate() const {
    return renameEdit_->text().trimmed();
}

void ImportDialog::refreshRenamePreview() {
    const bool on = renameBox_->isChecked();
    renameEdit_->setEnabled(on);
    renamePreview_->setVisible(on);

    if (!on || paths_.isEmpty() || renameTemplate().isEmpty()) {
        renamePreview_->setText(on ? tr("启用后此处预览新文件名。") : QString());
        return;
    }
    const std::string tmpl = renameTemplate().toStdString();
    const int shown =
        std::min(static_cast<int>(paths_.size()), static_cast<int>(5));
    QStringList preview;
    // The worker numbers files by selection order (1-based) and uses the source
    // file's modification date for {date}; mirror that here so the preview name
    // for an untouched file matches what the import will produce.
    for (int i = 0; i < shown; ++i) {
        lrc::RenameTemplateArgs a;
        a.tmpl = tmpl;
        a.base = QFileInfo(paths_[i]).completeBaseName().toStdString();
        a.index = i + 1;
        a.indexWidth = 2;
        a.date = QFileInfo(paths_[i])
                     .lastModified()
                     .toString(QStringLiteral("yyyy-MM-dd"))
                     .toStdString();
        const std::string rendered = lrc::renderRenameTemplate(a);
        preview << tr("%1 → %2")
                       .arg(QFileInfo(paths_[i]).fileName())
                       .arg(QString::fromStdString(rendered));
    }
    if (paths_.size() > shown)
        preview << tr("… 共 %1 个").arg(paths_.size());
    renamePreview_->setText(preview.join(QStringLiteral("\n")));
}
