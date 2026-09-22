#include "ui/PresetDialog.h"

#include "utils/PresetLibrary.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVariant>
#include <QVBoxLayout>

#include <filesystem>
#include <string>

namespace {
// Sanity bound for the preset list; a real user library is far smaller.
constexpr int kMaxPresets = 500;
} // namespace

PresetDialog::PresetDialog(lrc::PresetLibrary& library,
                           const lrc::Adjustments& current, QWidget* parent)
    : QDialog(parent), library_(library), current_(current) {
    setWindowTitle(tr("管理预设"));
    setMinimumWidth(380);
    auto* root = new QVBoxLayout(this);
    root->setSpacing(8);

    {  // top: name field + 保存为预设 button
        auto* createRow = new QHBoxLayout;
        createRow->setSpacing(6);
        nameEdit_ = new QLineEdit(this);
        nameEdit_->setObjectName(QStringLiteral("presetNameEdit"));
        nameEdit_->setPlaceholderText(tr("预设名称…"));
        createRow->addWidget(nameEdit_, 1);
        auto* createBtn = new QPushButton(tr("保存为预设"), this);
        createRow->addWidget(createBtn);
        root->addLayout(createRow);

        connect(createBtn, &QPushButton::clicked, this,
                &PresetDialog::onCreateClicked);
        connect(nameEdit_, &QLineEdit::returnPressed, this,
                &PresetDialog::onCreateClicked);
    }

    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("presetList"));
    root->addWidget(list_, 1);

    {  // bottom: apply / export / delete
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        auto* applyBtn = new QPushButton(tr("应用"), this);
        applyBtn->setDefault(true);
        auto* exportBtn = new QPushButton(tr("导出分享…"), this);
        auto* delBtn = new QPushButton(tr("删除"), this);
        row->addWidget(applyBtn);
        row->addWidget(exportBtn);
        row->addWidget(delBtn);
        row->addStretch(1);
        root->addLayout(row);

        connect(applyBtn, &QPushButton::clicked, this,
                &PresetDialog::onApplyClicked);
        connect(exportBtn, &QPushButton::clicked, this,
                &PresetDialog::onExportClicked);
        connect(delBtn, &QPushButton::clicked, this,
                &PresetDialog::onDeleteClicked);
        connect(list_, &QListWidget::itemDoubleClicked, this,
                [this](QListWidgetItem*) { onApplyClicked(); });
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    reload();
}

void PresetDialog::reload() {
    list_->clear();
    const auto names = library_.listPresets();
    int n = 0;
    for (const auto& name : names) {
        if (n >= kMaxPresets)
            break;
        auto* item = new QListWidgetItem(QString::fromStdString(name));
        item->setData(Qt::UserRole, QString::fromStdString(name));
        list_->addItem(item);
        ++n;
    }
    if (n == 0)
        list_->addItem(tr("（暂无预设）"));
}

void PresetDialog::okMessage(const QString& text) {
    QMessageBox::information(this, tr("管理预设"), text);
}

QString PresetDialog::selectedName() const {
    auto* item = list_->currentItem();
    if (!item || item->data(Qt::UserRole).isNull())
        return QString();
    return item->data(Qt::UserRole).toString();
}

void PresetDialog::onCreateClicked() {
    QString name = nameEdit_->text().trimmed();
    if (name.isEmpty()) {
        name = QInputDialog::getText(this, tr("保存为预设"),
                                     tr("预设名称:"), QLineEdit::Normal,
                                     tr("预设 %1").arg(list_->count() + 1));
        if (name.trimmed().isEmpty())
            return;
        name = name.trimmed();
    }
    if (!lrc::PresetLibrary::isValidName(name.toStdString())) {
        okMessage(tr("预设名称包含文件系统不允许的字符，请换一个名称。"));
        return;
    }
    // Save is an update: an existing preset with the same name is overwritten,
    // keeping preset names unique.
    if (library_.savePreset(name.toStdString(), current_)) {
        nameEdit_->clear();
        reload();
    } else {
        okMessage(tr("保存预设失败。"));
    }
}

void PresetDialog::onApplyClicked() {
    const QString name = selectedName();
    if (name.isEmpty())
        return;
    const auto preset = library_.loadPreset(name.toStdString());
    if (!preset) {
        okMessage(tr("无法读取该预设（文件可能已损坏或被移动）。"));
        return;
    }
    emit applied(*preset);
    accept();  // applying closes the dialog (the develop view now reflects it)
}

void PresetDialog::onDeleteClicked() {
    const QString name = selectedName();
    if (name.isEmpty())
        return;
    if (QMessageBox::question(this, tr("删除预设"),
                              tr("确定删除预设“%1”吗？").arg(name)) !=
        QMessageBox::Yes)
        return;
    if (library_.deletePreset(name.toStdString()))
        reload();
    else
        okMessage(tr("删除失败。"));
}

void PresetDialog::onExportClicked() {
    const QString name = selectedName();
    if (name.isEmpty())
        return;
    const QString suggested = QDir::homePath() + QStringLiteral("/") + name +
                              QStringLiteral(".lrp");
    QString path = QFileDialog::getSaveFileName(
        this, tr("导出预设"), suggested, tr("本应用预设 (*.lrp)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".lrp");
    if (library_.exportPreset(name.toStdString(),
                              std::filesystem::path(path.toStdWString())))
        okMessage(tr("已导出预设：%1").arg(QFileInfo(path).fileName()));
    else
        okMessage(tr("导出失败。"));
}
