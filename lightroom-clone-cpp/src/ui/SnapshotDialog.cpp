#include "ui/SnapshotDialog.h"

#include "catalog/CatalogDatabase.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVariant>
#include <QVBoxLayout>

// Max displayable snapshot labels per image; a sane upper bound for the list.
namespace {
constexpr int kMaxSnapshots = 500;
}

SnapshotDialog::SnapshotDialog(lrc::CatalogDatabase& catalog, qint64 imageId,
                               int variantId, QWidget* parent)
    : QDialog(parent),
      catalog_(catalog),
      imageId_(imageId),
      variantId_(variantId) {
    setWindowTitle(tr("快照"));
    setMinimumWidth(360);
    auto* root = new QVBoxLayout(this);
    root->setSpacing(8);

    {  // top: name field + 新建 button
        auto* createRow = new QHBoxLayout;
        createRow->setSpacing(6);
        nameEdit_ = new QLineEdit(this);
        nameEdit_->setPlaceholderText(tr("快照名称…"));
        createRow->addWidget(nameEdit_, 1);
        auto* createBtn = new QPushButton(tr("新建"), this);
        createRow->addWidget(createBtn);
        root->addLayout(createRow);

        connect(createBtn, &QPushButton::clicked, this,
                &SnapshotDialog::onCreateClicked);
        connect(nameEdit_, &QLineEdit::returnPressed, this,
                &SnapshotDialog::onCreateClicked);
    }

    list_ = new QListWidget(this);
    root->addWidget(list_, 1);

    {  // bottom: apply / rename / delete
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        auto* applyBtn = new QPushButton(tr("应用"), this);
        applyBtn->setDefault(true);
        auto* renameBtn = new QPushButton(tr("重命名"), this);
        auto* delBtn = new QPushButton(tr("删除"), this);
        row->addWidget(applyBtn);
        row->addWidget(renameBtn);
        row->addWidget(delBtn);
        row->addStretch(1);
        root->addLayout(row);

        connect(applyBtn, &QPushButton::clicked, this,
                &SnapshotDialog::onApplyClicked);
        connect(renameBtn, &QPushButton::clicked, this,
                &SnapshotDialog::onRenameClicked);
        connect(delBtn, &QPushButton::clicked, this,
                &SnapshotDialog::onDeleteClicked);
        connect(list_, &QListWidget::itemDoubleClicked, this,
                [this](QListWidgetItem*) { onApplyClicked(); });
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    reload();
}

void SnapshotDialog::reload() {
    list_->clear();
    const auto history = catalog_.listEditHistory(imageId_, variantId_);
    int n = 0;
    // Show named snapshots only; the automatic unlabelled undo history is
    // managed by the separate edit-history dialog.
    for (const auto& snap : history) {
        if (snap.name.empty())
            continue;
        if (n >= kMaxSnapshots)
            break;
        const QString label = QString::fromUtf8(snap.name.data(),
                                                int(snap.name.size())) +
                              "  ·  " + QString::fromStdString(snap.createdAt);
        auto* item = new QListWidgetItem(label);
        item->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(snap.id));
        list_->addItem(item);
        ++n;
    }
    if (n == 0)
        list_->addItem(tr("（暂无命名快照）"));
}

void SnapshotDialog::okMessage(const QString& text) {
    QMessageBox::information(this, tr("快照"), text);
}

void SnapshotDialog::onCreateClicked() {
    const QString raw = nameEdit_->text().trimmed();
    const QString name = raw.isEmpty()
                             ? QInputDialog::getText(this, tr("新建快照"),
                                                     tr("快照名称:"), QLineEdit::Normal,
                                                     tr("快照 %1")
                                                         .arg(list_->count() + 1))
                             : raw;
    if (name.trimmed().isEmpty())
        return;
    const auto adj = catalog_.loadAdjustments(imageId_, variantId_);
    if (!adj) {
        okMessage(tr("当前没有可保存的编辑参数。"));
        return;
    }
    if (catalog_.saveEditSnapshot(imageId_, *adj, variantId_,
                                  name.trimmed().toStdString()) > 0) {
        nameEdit_->clear();
        reload();
    } else {
        okMessage(tr("保存快照失败。"));
    }
}

void SnapshotDialog::onApplyClicked() {
    auto* item = list_->currentItem();
    if (!item || item->data(Qt::UserRole).isNull())
        return;
    const qlonglong snapId = item->data(Qt::UserRole).toLongLong();
    const auto adj = catalog_.loadEditSnapshot(imageId_, snapId, variantId_);
    if (!adj) {
        okMessage(tr("无法读取该快照。"));
        return;
    }
    emit applied(*adj);
    accept();  // applying closes the dialog (the develop view now reflects it)
}

void SnapshotDialog::onRenameClicked() {
    auto* item = list_->currentItem();
    if (!item || item->data(Qt::UserRole).isNull())
        return;
    const qlonglong snapId = item->data(Qt::UserRole).toLongLong();
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, tr("重命名快照"), tr("新名称:"),
                              QLineEdit::Normal, item->text(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    if (catalog_.renameSnapshot(imageId_, snapId, name.trimmed().toStdString(),
                                variantId_))
        reload();
    else
        okMessage(tr("重命名失败。"));
}

void SnapshotDialog::onDeleteClicked() {
    auto* item = list_->currentItem();
    if (!item || item->data(Qt::UserRole).isNull())
        return;
    const qlonglong snapId = item->data(Qt::UserRole).toLongLong();
    if (QMessageBox::question(this, tr("删除快照"),
                              tr("确定删除所选快照吗？")) != QMessageBox::Yes)
        return;
    if (catalog_.deleteEditSnapshot(imageId_, snapId, variantId_))
        reload();
    else
        okMessage(tr("删除失败。"));
}