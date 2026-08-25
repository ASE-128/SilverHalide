#include "ui/CollectionsDialog.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QVBoxLayout>

CollectionsDialog::CollectionsDialog(lrc::CatalogDatabase* catalog,
                                     QWidget* parent)
    : QDialog(parent), catalog_(catalog) {
    setWindowTitle(tr("管理收藏夹"));
    setMinimumSize(680, 420);

    auto* root = new QHBoxLayout(this);
    root->setSpacing(8);

    // Left column: collections list + management buttons.
    auto* left = new QVBoxLayout;
    left->setSpacing(4);
    auto* leftCaption = new QLabel(tr("收藏夹"), this);
    leftCaption->setProperty("caption", true);
    left->addWidget(leftCaption);

    collectionsList_ = new QListWidget(this);
    left->addWidget(collectionsList_, 1);

    auto* newButton = new QPushButton(tr("新建"), this);
    renameButton_ = new QPushButton(tr("重命名"), this);
    deleteButton_ = new QPushButton(tr("删除"), this);
    auto* leftButtons = new QHBoxLayout;
    leftButtons->addWidget(newButton);
    leftButtons->addWidget(renameButton_);
    leftButtons->addWidget(deleteButton_);
    left->addLayout(leftButtons);
    root->addLayout(left, 1);

    // Right column: selected collection title, member list, member buttons.
    auto* right = new QVBoxLayout;
    right->setSpacing(4);
    collectionTitle_ = new QLabel(tr("（未选择）"), this);
    collectionTitle_->setProperty("caption", true);
    right->addWidget(collectionTitle_);

    membersList_ = new QListWidget(this);
    right->addWidget(membersList_, 2);

    addButton_ = new QPushButton(tr("添加照片…"), this);
    removeButton_ = new QPushButton(tr("移除选中"), this);
    auto* rightButtons = new QHBoxLayout;
    rightButtons->addWidget(addButton_);
    rightButtons->addStretch(1);
    rightButtons->addWidget(removeButton_);
    right->addLayout(rightButtons);
    root->addLayout(right, 2);

    // Initially disabled; refreshCollections() -> onSelectionChanged() enables
    // them once a collection is selected.
    renameButton_->setEnabled(false);
    deleteButton_->setEnabled(false);
    addButton_->setEnabled(false);
    removeButton_->setEnabled(false);

    refreshCollections();

    connect(collectionsList_, &QListWidget::itemSelectionChanged, this,
            &CollectionsDialog::onSelectionChanged);
    connect(newButton, &QPushButton::clicked, this,
            &CollectionsDialog::onCreateCollection);
    connect(renameButton_, &QPushButton::clicked, this,
            &CollectionsDialog::onRenameCollection);
    connect(deleteButton_, &QPushButton::clicked, this,
            &CollectionsDialog::onDeleteCollection);
    connect(addButton_, &QPushButton::clicked, this,
            &CollectionsDialog::onAddPhotos);
    connect(removeButton_, &QPushButton::clicked, this,
            &CollectionsDialog::onRemoveSelected);
}

void CollectionsDialog::refreshCollections() {
    const QSignalBlocker blocker(collectionsList_);
    collectionsList_->clear();
    for (const lrc::Collection& collection : catalog_->listCollections()) {
        auto* item = new QListWidgetItem(
            QString::fromStdString(collection.name), collectionsList_);
        item->setData(Qt::UserRole, QVariant::fromValue(collection.id));
    }
    if (collectionsList_->count() == 0) {
        auto* empty = new QListWidgetItem(tr("（无收藏夹）"), collectionsList_);
        empty->setFlags(Qt::NoItemFlags);
    } else if (collectionsList_->currentItem() == nullptr) {
        collectionsList_->setCurrentRow(0);
    }
    onSelectionChanged();
}

void CollectionsDialog::refreshMembers() {
    const QSignalBlocker blocker(membersList_);
    membersList_->clear();
    const int64_t id = currentCollectionId();
    if (id <= 0)
        return;
    for (const lrc::ImageRecord& record : catalog_->listImagesInCollection(id)) {
        auto* item = new QListWidgetItem(
            QFileInfo(QString::fromStdString(record.filePath)).fileName(),
            membersList_);
        item->setData(Qt::UserRole, QVariant::fromValue(record.id));
    }
    if (membersList_->count() == 0) {
        auto* empty = new QListWidgetItem(tr("（无成员）"), membersList_);
        empty->setFlags(Qt::NoItemFlags);
    }
}

int64_t CollectionsDialog::currentCollectionId() const {
    QListWidgetItem* item = collectionsList_->currentItem();
    if (!item)
        return 0;
    return item->data(Qt::UserRole).toLongLong();
}

QString CollectionsDialog::collectionName(int64_t id) const {
    for (const lrc::Collection& collection : catalog_->listCollections()) {
        if (collection.id == id)
            return QString::fromStdString(collection.name);
    }
    return tr("（未选择）");
}

void CollectionsDialog::onSelectionChanged() {
    const int64_t id = currentCollectionId();
    collectionTitle_->setText(id > 0 ? collectionName(id) : tr("（未选择）"));
    refreshMembers();
    const bool hasCollection = id > 0;
    renameButton_->setEnabled(hasCollection);
    deleteButton_->setEnabled(hasCollection);
    addButton_->setEnabled(hasCollection);
    removeButton_->setEnabled(hasCollection);
}

void CollectionsDialog::onCreateCollection() {
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("新建收藏夹"), tr("收藏夹名称："), QLineEdit::Normal,
        QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    const int64_t id = catalog_->createCollection(name.trimmed().toStdString());
    if (id <= 0) {
        QMessageBox::warning(this, tr("新建收藏夹"), tr("创建收藏夹失败。"));
        return;
    }
    refreshCollections();
    for (int i = 0; i < collectionsList_->count(); ++i) {
        if (collectionsList_->item(i)->data(Qt::UserRole).toLongLong() == id) {
            collectionsList_->setCurrentRow(i);
            break;
        }
    }
    emit collectionsChanged();
}

void CollectionsDialog::onRenameCollection() {
    const int64_t id = currentCollectionId();
    if (id <= 0)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("重命名收藏夹"), tr("收藏夹名称："), QLineEdit::Normal,
        collectionName(id), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    if (!catalog_->renameCollection(id, name.trimmed().toStdString())) {
        QMessageBox::warning(this, tr("重命名收藏夹"), tr("重命名失败。"));
        return;
    }
    refreshCollections();
    for (int i = 0; i < collectionsList_->count(); ++i) {
        if (collectionsList_->item(i)->data(Qt::UserRole).toLongLong() == id) {
            collectionsList_->setCurrentRow(i);
            break;
        }
    }
    emit collectionsChanged();
}

void CollectionsDialog::onDeleteCollection() {
    const int64_t id = currentCollectionId();
    if (id <= 0)
        return;
    const QString name = collectionName(id);
    if (QMessageBox::question(
            this, tr("删除收藏夹"),
            tr("删除收藏夹「%1」？\n其中的照片不会被删除。").arg(name)) !=
        QMessageBox::Yes)
        return;
    if (!catalog_->deleteCollection(id)) {
        QMessageBox::warning(this, tr("删除收藏夹"), tr("删除失败。"));
        return;
    }
    refreshCollections();
    emit collectionsChanged();
}

void CollectionsDialog::onAddPhotos() {
    const int64_t collectionId = currentCollectionId();
    if (collectionId <= 0)
        return;

    // Members already in the collection are excluded from the picker.
    QSet<int64_t> existing;
    for (const lrc::ImageRecord& record :
         catalog_->listImagesInCollection(collectionId))
        existing.insert(record.id);

    QDialog picker(this);
    picker.setWindowTitle(
        tr("添加照片到「%1」").arg(collectionName(collectionId)));
    picker.setMinimumSize(480, 380);
    auto* pickerLayout = new QVBoxLayout(&picker);
    auto* list = new QListWidget(&picker);
    pickerLayout->addWidget(list, 1);
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                             Qt::Horizontal, &picker);
    pickerLayout->addWidget(buttons);

    int candidateCount = 0;
    for (const lrc::ImageRecord& record : catalog_->listImages()) {
        if (existing.contains(record.id))
            continue;
        auto* item = new QListWidgetItem(
            QFileInfo(QString::fromStdString(record.filePath)).fileName(),
            list);
        item->setData(Qt::UserRole, QVariant::fromValue(record.id));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        ++candidateCount;
    }
    if (candidateCount == 0) {
        auto* empty = new QListWidgetItem(tr("（没有可添加的照片）"), list);
        empty->setFlags(Qt::NoItemFlags);
    }

    connect(buttons, &QDialogButtonBox::accepted, &picker, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &picker, &QDialog::reject);
    if (picker.exec() != QDialog::Accepted)
        return;

    bool changed = false;
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem* item = list->item(i);
        if (item->flags().testFlag(Qt::ItemIsUserCheckable) &&
            item->checkState() == Qt::Checked) {
            const int64_t imageId = item->data(Qt::UserRole).toLongLong();
            if (catalog_->addToCollection(collectionId, imageId))
                changed = true;
        }
    }
    if (changed) {
        refreshMembers();
        emit collectionsChanged();
    }
}

void CollectionsDialog::onRemoveSelected() {
    const int64_t collectionId = currentCollectionId();
    if (collectionId <= 0)
        return;
    const QList<QListWidgetItem*> selected = membersList_->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::information(this, tr("移除照片"),
                                 tr("请先在右侧列表中选择要移除的照片。"));
        return;
    }
    bool changed = false;
    for (QListWidgetItem* item : selected) {
        const int64_t imageId = item->data(Qt::UserRole).toLongLong();
        if (catalog_->removeFromCollection(collectionId, imageId))
            changed = true;
    }
    if (changed) {
        refreshMembers();
        emit collectionsChanged();
    }
}
