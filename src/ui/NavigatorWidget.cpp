#include "ui/NavigatorWidget.h"

#include <QComboBox>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace {

constexpr int kRoleKind = Qt::UserRole;  // "all" | "folder" | "library" | ...
constexpr int kRolePath = Qt::UserRole + 1;
constexpr int kRoleCollectionId = Qt::UserRole + 2;      // int64 collection id
constexpr int kRoleImageId = Qt::UserRole + 3;           // int64 image id
constexpr int kRoleVariantId = Qt::UserRole + 4;         // int variant id
constexpr int kRoleKeywordId = Qt::UserRole + 5;         // int64 keyword id
constexpr int kRoleSmartCollectionId = Qt::UserRole + 6; // int64 smart id

// Recursive search for the rebuilt item matching the remembered selection
// (kind + path, or collection / variant / keyword / smart-collection id).
// Returns nullptr when nothing matches.
QTreeWidgetItem* findMatchingItem(QTreeWidgetItem* item, const QString& kind,
                                  const QString& path, qint64 collectionId,
                                  qint64 imageId, int variantId,
                                  qint64 keywordId, qint64 smartCollectionId) {
    if (item->data(0, kRoleKind).toString() == kind) {
        if (kind == QStringLiteral("collection"))
            return item->data(0, kRoleCollectionId).toLongLong() == collectionId
                       ? item
                       : nullptr;
        if (kind == QStringLiteral("smart_collection"))
            return item->data(0, kRoleSmartCollectionId).toLongLong() ==
                           smartCollectionId
                       ? item
                       : nullptr;
        if (kind == QStringLiteral("virtual_copy"))
            return item->data(0, kRoleImageId).toLongLong() == imageId &&
                           item->data(0, kRoleVariantId).toInt() == variantId
                       ? item
                       : nullptr;
        if (kind == QStringLiteral("keyword"))
            return item->data(0, kRoleKeywordId).toLongLong() == keywordId
                       ? item
                       : nullptr;
        if (kind == QStringLiteral("all") ||
            item->data(0, kRolePath).toString() == path)
            return item;
    }
    for (int i = 0; i < item->childCount(); ++i) {
        if (QTreeWidgetItem* match =
                findMatchingItem(item->child(i), kind, path, collectionId,
                                 imageId, variantId, keywordId,
                                 smartCollectionId))
            return match;
    }
    return nullptr;
}

} // namespace

NavigatorWidget::NavigatorWidget(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(2);

    // Filter row (rating / flag / label colour), LR-style.
    auto* filterRow = new QHBoxLayout;
    filterRow->setSpacing(4);
    ratingCombo_ = new QComboBox(this);
    ratingCombo_->setToolTip(tr("按评分筛选"));
    ratingCombo_->addItem(tr("全部"), 0);
    ratingCombo_->addItem(tr("≥1星"), 1);
    ratingCombo_->addItem(tr("≥2星"), 2);
    ratingCombo_->addItem(tr("≥3星"), 3);
    ratingCombo_->addItem(tr("≥4星"), 4);
    ratingCombo_->addItem(tr("≥5星"), 5);
    filterRow->addWidget(ratingCombo_, 1);

    flagCombo_ = new QComboBox(this);
    flagCombo_->setToolTip(tr("按旗标筛选"));
    flagCombo_->addItem(tr("全部"), 0);
    flagCombo_->addItem(tr("已选取"), 1);
    flagCombo_->addItem(tr("无旗标"), 2);
    flagCombo_->addItem(tr("已拒绝"), -1);
    filterRow->addWidget(flagCombo_, 1);

    labelColorCombo_ = new QComboBox(this);
    labelColorCombo_->setToolTip(tr("按标签颜色筛选"));
    labelColorCombo_->addItem(tr("全部"), 0);
    labelColorCombo_->addItem(tr("红"), 1);
    labelColorCombo_->addItem(tr("橙"), 2);
    labelColorCombo_->addItem(tr("黄"), 3);
    labelColorCombo_->addItem(tr("绿"), 4);
    labelColorCombo_->addItem(tr("青"), 5);
    labelColorCombo_->addItem(tr("蓝"), 6);
    labelColorCombo_->addItem(tr("紫"), 7);
    filterRow->addWidget(labelColorCombo_, 1);
    root->addLayout(filterRow);

    tree_ = new QTreeWidget(this);
    tree_->setHeaderHidden(true);
    tree_->setColumnCount(1);
    tree_->setIndentation(14);
    root->addWidget(tree_, 1);

    // New-smart-collection entry, shown once the group exists.
    newSmartButton_ = new QPushButton(tr("＋ 新建智能收藏夹…"), this);
    newSmartButton_->setFlat(true);
    newSmartButton_->hide();
    root->addWidget(newSmartButton_);
    connect(newSmartButton_, &QPushButton::clicked, this,
            [this]() { emit newSmartCollectionRequested(); });

    statsLabel_ = new QLabel(tr("0 张图片"), this);
    statsLabel_->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    root->addWidget(statsLabel_);

    allPhotos_ = new QTreeWidgetItem(tree_, {tr("所有照片")});
    allPhotos_->setData(0, kRoleKind, QStringLiteral("all"));
    folderRoot_ = new QTreeWidgetItem(tree_, {tr("文件夹")});
    folderRoot_->setFlags(folderRoot_->flags() & ~Qt::ItemIsSelectable);
    tree_->expandAll();

    connect(tree_, &QTreeWidget::itemSelectionChanged, this,
            [this]() {
                const auto items = tree_->selectedItems();
                if (!items.isEmpty())
                    onItemSelected(items.first(), 0);
            });
    connect(ratingCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { onFilterChanged(); });
    connect(flagCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { onFilterChanged(); });
    connect(labelColorCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { onFilterChanged(); });
}

void NavigatorWidget::setFolders(const QStringList& folders) {
    rememberSelection();  // 保存选中态以在重建后恢复 (save selection).
    const QSignalBlocker blocker(tree_);
    // Remove previous folder children.
    for (QTreeWidgetItem* child : folderRoot_->takeChildren())
        delete child;
    for (const QString& folder : folders) {
        auto* item = new QTreeWidgetItem(folderRoot_, {folder});
        item->setData(0, kRoleKind, QStringLiteral("folder"));
        item->setData(0, kRolePath, folder);
        item->setToolTip(0, folder);
    }
    tree_->expandAll();
    restoreSelection();  // 恢复重建前的选中项 (restore selection).
}

void NavigatorWidget::setLibrary(const QString& libraryRoot,
                                 const QStringList& subfolders) {
    rememberSelection();  // 保存选中态以在重建后恢复 (save selection).
    const QSignalBlocker blocker(tree_);

    // Empty library root removes the whole "目录图片库" group (empty library).
    if (libraryRoot.isEmpty()) {
        libraryRootPath_.clear();
        if (libraryRoot_ != nullptr) {
            for (QTreeWidgetItem* child : libraryRoot_->takeChildren())
                delete child;
            const int idx = tree_->indexOfTopLevelItem(libraryRoot_);
            if (idx >= 0)
                delete tree_->takeTopLevelItem(idx);
            libraryRoot_ = nullptr;
        }
        tree_->expandAll();
        restoreSelection();  // 库分组被移除时若选中项已不存在则保持原状 (no-op).
        return;
    }

    // Create the group top-level item right after the folder root on first use.
    if (libraryRoot_ == nullptr) {
        libraryRoot_ = new QTreeWidgetItem;
        libraryRoot_->setData(0, kRoleKind, QStringLiteral("library"));
        insertTopLevelInOrder(libraryRoot_);
    } else {
        // Rebuild: drop the previous subfolders.
        for (QTreeWidgetItem* child : libraryRoot_->takeChildren())
            delete child;
    }

    libraryRootPath_ = libraryRoot;
    libraryRoot_->setText(0, tr("目录图片库"));
    libraryRoot_->setData(0, kRolePath, libraryRoot);
    libraryRoot_->setToolTip(0, libraryRoot);

    if (subfolders.isEmpty()) {
        // Placeholder child: selecting it must not emit folderSelected.
        auto* empty = new QTreeWidgetItem(libraryRoot_, {tr("（空）")});
        empty->setData(0, kRoleKind, QStringLiteral("library_empty"));
    } else {
        for (const QString& relative : subfolders) {
            auto* item = new QTreeWidgetItem(libraryRoot_, {relative});
            item->setData(0, kRoleKind, QStringLiteral("library_sub"));
            item->setData(0, kRolePath, relative);
            item->setToolTip(0, relative);
        }
    }
    tree_->expandAll();
    restoreSelection();  // 恢复重建前的选中项 (restore selection).
}

void NavigatorWidget::setCollections(const QVector<lrc::Collection>& collections) {
    rememberSelection();  // 保存选中态以在重建后恢复 (save selection).
    const QSignalBlocker blocker(tree_);

    // Create the group top-level item on first use (ordered after the library
    // and virtual-copy groups, before the smart-collection group).
    if (collectionsRoot_ == nullptr) {
        collectionsRoot_ = new QTreeWidgetItem;
        collectionsRoot_->setData(0, kRoleKind, QStringLiteral("collections"));
        insertTopLevelInOrder(collectionsRoot_);
    } else {
        // Rebuild: drop the previous collection children.
        for (QTreeWidgetItem* child : collectionsRoot_->takeChildren())
            delete child;
    }

    collectionsRoot_->setText(0, tr("收藏夹"));

    if (collections.isEmpty()) {
        // Placeholder child: selecting it must not emit collectionSelected.
        auto* empty = new QTreeWidgetItem(collectionsRoot_, {tr("（空）")});
        empty->setData(0, kRoleKind, QStringLiteral("collections_empty"));
    } else {
        for (const lrc::Collection& collection : collections) {
            auto* item = new QTreeWidgetItem(
                collectionsRoot_, {QString::fromStdString(collection.name)});
            item->setData(0, kRoleKind, QStringLiteral("collection"));
            item->setData(0, kRoleCollectionId,
                          QVariant::fromValue(collection.id));
            item->setToolTip(0, QString::fromStdString(collection.name));
        }
    }
    tree_->expandAll();
    restoreSelection();  // 恢复重建前的选中项 (restore selection).
}

void NavigatorWidget::setVirtualCopies(
    const std::vector<std::pair<int64_t, lrc::EditVariant>>& virtualCopies) {
    rememberSelection();  // 保存选中态以在重建后恢复 (save selection).
    const QSignalBlocker blocker(tree_);

    if (virtualCopiesRoot_ == nullptr) {
        virtualCopiesRoot_ = new QTreeWidgetItem;
        virtualCopiesRoot_->setData(0, kRoleKind,
                                    QStringLiteral("virtualcopies"));
        insertTopLevelInOrder(virtualCopiesRoot_);
    } else {
        // Rebuild: drop the previous virtual-copy children.
        for (QTreeWidgetItem* child : virtualCopiesRoot_->takeChildren())
            delete child;
    }
    virtualCopiesRoot_->setText(0, tr("虚拟副本"));

    if (virtualCopies.empty()) {
        auto* empty = new QTreeWidgetItem(virtualCopiesRoot_, {tr("（空）")});
        empty->setData(0, kRoleKind, QStringLiteral("virtualcopies_empty"));
    } else {
        for (const auto& vc : virtualCopies) {
            const lrc::EditVariant& variant = vc.second;
            QString label;
            if (variant.isMaster)
                label = tr("母版");
            else if (!variant.name.empty())
                label = QString::fromStdString(variant.name);
            else
                label = tr("副本 %1").arg(variant.variantId);
            auto* item = new QTreeWidgetItem(virtualCopiesRoot_, {label});
            item->setData(0, kRoleKind, QStringLiteral("virtual_copy"));
            item->setData(0, kRoleImageId, QVariant::fromValue(vc.first));
            item->setData(0, kRoleVariantId,
                          QVariant::fromValue(variant.variantId));
            item->setToolTip(0, tr("图片 %1 · 变体 %2")
                                    .arg(vc.first)
                                    .arg(variant.variantId));
        }
    }
    tree_->expandAll();
    restoreSelection();  // 恢复重建前的选中项 (restore selection).
}

void NavigatorWidget::setKeywords(const std::vector<lrc::KeywordInfo>& keywords) {
    rememberSelection();  // 保存选中态以在重建后恢复 (save selection).
    const QSignalBlocker blocker(tree_);

    if (keywordsRoot_ == nullptr) {
        keywordsRoot_ = new QTreeWidgetItem;
        keywordsRoot_->setData(0, kRoleKind, QStringLiteral("keywords"));
        insertTopLevelInOrder(keywordsRoot_);
    } else {
        // Rebuild: drop the previous keyword children.
        for (QTreeWidgetItem* child : keywordsRoot_->takeChildren())
            delete child;
    }
    keywordsRoot_->setText(0, tr("关键字列表"));

    if (keywords.empty()) {
        auto* empty = new QTreeWidgetItem(keywordsRoot_, {tr("（空）")});
        empty->setData(0, kRoleKind, QStringLiteral("keywords_empty"));
    } else {
        for (const lrc::KeywordInfo& kw : keywords) {
            const QString path = QString::fromStdString(kw.fullPath);
            const QString display = path.isEmpty()
                                        ? QString::fromStdString(kw.name)
                                        : path;
            const QString label =
                QStringLiteral("%1 (%2)").arg(display).arg(kw.photoCount);
            auto* item = new QTreeWidgetItem(keywordsRoot_, {label});
            item->setData(0, kRoleKind, QStringLiteral("keyword"));
            item->setData(0, kRolePath, path);
            item->setData(0, kRoleKeywordId, QVariant::fromValue(kw.id));
            item->setToolTip(0, label);
        }
    }
    tree_->expandAll();
    restoreSelection();  // 恢复重建前的选中项 (restore selection).
}

void NavigatorWidget::setSmartCollections(
    const std::vector<lrc::SmartCollection>& smartCollections) {
    rememberSelection();  // 保存选中态以在重建后恢复 (save selection).
    const QSignalBlocker blocker(tree_);

    if (smartCollectionsRoot_ == nullptr) {
        smartCollectionsRoot_ = new QTreeWidgetItem;
        smartCollectionsRoot_->setData(0, kRoleKind,
                                       QStringLiteral("smartcollections"));
        insertTopLevelInOrder(smartCollectionsRoot_);
        newSmartButton_->show();  // 入口按钮随分组一起出现 (button follows group).
    } else {
        // Rebuild: drop the previous smart-collection children.
        for (QTreeWidgetItem* child : smartCollectionsRoot_->takeChildren())
            delete child;
    }
    smartCollectionsRoot_->setText(0, tr("智能收藏夹"));

    if (smartCollections.empty()) {
        auto* empty = new QTreeWidgetItem(smartCollectionsRoot_, {tr("（空）")});
        empty->setData(0, kRoleKind, QStringLiteral("smartcollections_empty"));
    } else {
        for (const lrc::SmartCollection& sc : smartCollections) {
            auto* item = new QTreeWidgetItem(smartCollectionsRoot_,
                                             {QString::fromStdString(sc.name)});
            item->setData(0, kRoleKind, QStringLiteral("smart_collection"));
            item->setData(0, kRoleSmartCollectionId,
                          QVariant::fromValue(sc.id));
            item->setToolTip(0, QString::fromStdString(sc.name));
        }
    }
    tree_->expandAll();
    restoreSelection();  // 恢复重建前的选中项 (restore selection).
}

void NavigatorWidget::setStatistics(int imageCount, int ratedCount) {
    statsLabel_->setText(tr("%1 张图片 · %2 已评分")
                             .arg(imageCount)
                             .arg(ratedCount));
}

void NavigatorWidget::rememberSelection() {
    // Remember what was selected before the rebuild, keeping only selectable and
    // meaningful kinds.
    prevKind_.clear();
    prevPath_.clear();
    prevCollectionId_ = -1;
    prevImageId_ = -1;
    prevVariantId_ = -1;
    prevKeywordId_ = -1;
    prevSmartCollectionId_ = -1;
    const auto items = tree_->selectedItems();
    if (items.isEmpty())
        return;
    const QString kind = items.first()->data(0, kRoleKind).toString();
    if (kind != QStringLiteral("all") && kind != QStringLiteral("folder") &&
        kind != QStringLiteral("library") &&
        kind != QStringLiteral("library_sub") &&
        kind != QStringLiteral("collection") &&
        kind != QStringLiteral("virtual_copy") &&
        kind != QStringLiteral("keyword") &&
        kind != QStringLiteral("smart_collection"))
        return;  // 占位项/未知 kind 不记录 (skip placeholders).
    prevKind_ = kind;
    prevPath_ = items.first()->data(0, kRolePath).toString();
    prevCollectionId_ = items.first()->data(0, kRoleCollectionId).toLongLong();
    prevImageId_ = items.first()->data(0, kRoleImageId).toLongLong();
    prevVariantId_ = items.first()->data(0, kRoleVariantId).toInt();
    prevKeywordId_ = items.first()->data(0, kRoleKeywordId).toLongLong();
    prevSmartCollectionId_ =
        items.first()->data(0, kRoleSmartCollectionId).toLongLong();
}

void NavigatorWidget::restoreSelection() {
    // Restore the previous selection on the rebuilt tree; QSignalBlocker prevents
    // any signal replay.
    if (prevKind_.isEmpty())
        return;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        if (QTreeWidgetItem* item =
                findMatchingItem(tree_->topLevelItem(i), prevKind_, prevPath_,
                                 prevCollectionId_, prevImageId_, prevVariantId_,
                                 prevKeywordId_, prevSmartCollectionId_)) {
            tree_->setCurrentItem(item);
            return;
        }
    }
}

int NavigatorWidget::groupPriority(const QString& kind) const {
    if (kind == QStringLiteral("library"))
        return 1;
    if (kind == QStringLiteral("virtualcopies"))
        return 2;
    if (kind == QStringLiteral("collections"))
        return 3;
    if (kind == QStringLiteral("smartcollections"))
        return 4;
    if (kind == QStringLiteral("keywords"))
        return 5;
    return 0;  // "all" / "folder" roots stay at the top (top of the tree).
}

void NavigatorWidget::insertTopLevelInOrder(QTreeWidgetItem* root) {
    const int prio = groupPriority(root->data(0, kRoleKind).toString());
    int insertAt = tree_->topLevelItemCount();
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        if (groupPriority(
                tree_->topLevelItem(i)->data(0, kRoleKind).toString()) > prio) {
            insertAt = i;
            break;
        }
    }
    tree_->insertTopLevelItem(insertAt, root);
}

void NavigatorWidget::onFilterChanged() {
    const int minRating = ratingCombo_->currentData().toInt();
    const int flagFilter = flagCombo_->currentData().toInt();
    const int labelColorFilter = labelColorCombo_->currentData().toInt();
    emit filtersChanged(minRating, flagFilter, labelColorFilter);
}

void NavigatorWidget::onItemSelected(QTreeWidgetItem* item, int) {
    const QString kind = item->data(0, kRoleKind).toString();
    if (kind == QStringLiteral("all"))
        emit allPhotosSelected();
    else if (kind == QStringLiteral("folder"))
        emit folderSelected(item->data(0, kRolePath).toString());
    else if (kind == QStringLiteral("library"))
        emit folderSelected(item->data(0, kRolePath).toString());
    else if (kind == QStringLiteral("library_sub"))
        emit folderSelected(libraryRootPath_ + QStringLiteral("/") +
                            item->data(0, kRolePath).toString());
    else if (kind == QStringLiteral("collection"))
        emit collectionSelected(
            static_cast<int>(item->data(0, kRoleCollectionId).toLongLong()));
    else if (kind == QStringLiteral("virtual_copy"))
        emit virtualCopySelected(item->data(0, kRoleImageId).toLongLong(),
                                 item->data(0, kRoleVariantId).toInt());
    else if (kind == QStringLiteral("keyword"))
        emit keywordSelected(item->data(0, kRoleKeywordId).toLongLong(),
                             item->data(0, kRolePath).toString());
    else if (kind == QStringLiteral("smart_collection"))
        emit smartCollectionSelected(
            item->data(0, kRoleSmartCollectionId).toLongLong());
    // kind "library_empty" / "collections_empty" / "virtualcopies_empty" /
    // "keywords_empty" / "smartcollections_empty" (placeholders) are
    // intentionally ignored (no signal).
}

void NavigatorWidget::contextMenuEvent(QContextMenuEvent* event) {
    // Right-clicking the smart-collections group creates a new one.
    QTreeWidgetItem* item =
        tree_->itemAt(tree_->viewport()->mapFromGlobal(event->globalPos()));
    QTreeWidgetItem* group = item;
    while (group && group->parent())
        group = group->parent();
    if (group != smartCollectionsRoot_)
        return;
    QMenu menu(this);
    if (menu.addAction(tr("新建智能收藏夹…")) == menu.exec(event->globalPos()))
        emit newSmartCollectionRequested();
}
