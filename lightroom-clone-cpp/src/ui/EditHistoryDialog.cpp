#include "ui/EditHistoryDialog.h"

#include <QDialogButtonBox>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "catalog/CatalogDatabase.h"
#include "engine/Adjustments.h"

EditHistoryDialog::EditHistoryDialog(lrc::CatalogDatabase& catalog,
                                     int64_t imageId, const QString& imageName,
                                     QWidget* parent)
    : QDialog(parent), catalog_(catalog), imageId_(imageId) {
    setWindowTitle(tr("编辑历史 — %1").arg(imageName));
    setMinimumWidth(380);

    auto* root = new QVBoxLayout(this);
    root->setSpacing(8);

    list_ = new QListWidget(this);
    root->addWidget(list_, 1);

    auto* buttons = new QDialogButtonBox(this);
    restoreButton_ =
        buttons->addButton(tr("恢复"), QDialogButtonBox::AcceptRole);
    QPushButton* closeButton = buttons->addButton(QDialogButtonBox::Close);
    root->addWidget(buttons);

    refresh();

    restoreButton_->setEnabled(false);
    connect(list_, &QListWidget::itemSelectionChanged, this, [this] {
        restoreButton_->setEnabled(list_->currentItem() != nullptr);
    });
    connect(list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { onRestore(); });
    connect(restoreButton_, &QPushButton::clicked, this,
            &EditHistoryDialog::onRestore);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
}

std::optional<lrc::Adjustments> EditHistoryDialog::restored() const {
    return restored_;
}

void EditHistoryDialog::refresh() {
    list_->clear();
    // listEditHistory returns newest last; reverse so newest shows on top.
    auto snapshots = catalog_.listEditHistory(imageId_);
    for (auto it = snapshots.rbegin(); it != snapshots.rend(); ++it) {
        const lrc::EditSnapshot& snapshot = *it;
        auto* item = new QListWidgetItem(
            QStringLiteral("#%1  %2")
                .arg(snapshot.id)
                .arg(QString::fromStdString(snapshot.createdAt)),
            list_);
        item->setData(Qt::UserRole, QVariant::fromValue(snapshot.id));
    }
}

void EditHistoryDialog::onRestore() {
    QListWidgetItem* item = list_->currentItem();
    if (!item)
        return;

    const int64_t snapshotId = item->data(Qt::UserRole).toLongLong();
    auto adjustments = catalog_.loadEditSnapshot(imageId_, snapshotId);
    if (!adjustments) {
        QMessageBox::warning(this, tr("编辑历史"), tr("无法读取该快照。"));
        return;
    }
    // 持久化 + 撤销 + 预览统一交给 commitAdjustments,恢复只写一条快照 (persistence,
    // undo and preview all go through MainWindow::commitAdjustments, so a restore
    // writes exactly one snapshot, not two).
    restored_ = *adjustments;
    accept();
}
