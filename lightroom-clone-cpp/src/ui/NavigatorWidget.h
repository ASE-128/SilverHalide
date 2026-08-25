#pragma once

// Left navigation panel: "All Photos", the imported folders, rating / flag /
// label-colour filters, a "收藏夹" (collections) group, and a small statistics
// footer. Selecting an entry emits a signal that filters the filmstrip.
// M6/M8/M9: virtual-copy, keyword-list and smart-collection groups.

#include <QStringList>
#include <QVector>
#include <QWidget>

#include <cstdint>
#include <utility>
#include <vector>

#include "catalog/CatalogDatabase.h"

class QComboBox;
class QContextMenuEvent;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

class NavigatorWidget : public QWidget {
    Q_OBJECT
public:
    explicit NavigatorWidget(QWidget* parent = nullptr);

    void setFolders(const QStringList& folders);
    // 显示受管图片库根及其下的子文件夹（相对路径，如 "2026/2026-08-11"）.
    // libraryRoot 是库根绝对路径；subfolders 是相对库根的目录列表（升序、去重）.
    // 空列表表示空库；传空 libraryRoot 时清除整个库分组.
    void setLibrary(const QString& libraryRoot, const QStringList& subfolders);
    // 显示「收藏夹」分组，子项为各集合；空列表仅保留占位子项 (placeholder).
    void setCollections(const QVector<lrc::Collection>& collections);
    void setStatistics(int imageCount, int ratedCount);

    // M6: 填充「虚拟副本」分组. 每项为 (imageId, variant)；显示 variant.name
    // (母版为空时显示「母版」). MainWindow 可预先填充 name 以区分不同图片.
    void setVirtualCopies(
        const std::vector<std::pair<int64_t, lrc::EditVariant>>& virtualCopies);
    // M8: 填充「关键字列表」分组，显示全路径与照片数 (如 "风光/冰岛/瀑布 (42)").
    void setKeywords(const std::vector<lrc::KeywordInfo>& keywords);
    // M9: 填充「智能收藏夹」分组；空列表仅保留占位子项 (placeholder).
    void setSmartCollections(
        const std::vector<lrc::SmartCollection>& smartCollections);

signals:
    void allPhotosSelected();
    void folderSelected(const QString& folderPath);
    // Rating filter 0..5 (0 = any). Flag filter: 0 = any, 1 = picked,
    // -1 = rejected, 2 = no flag. Label-colour filter: 0 = any, 1..7.
    void filtersChanged(int minRating, int flagFilter, int labelColorFilter);
    void collectionSelected(int collectionId);
    // M6: 点击虚拟副本项 (imageId, variantId).
    void virtualCopySelected(int64_t imageId, int variantId);
    // M8: 点击关键字项 (keywordId, 全路径 fullPath).
    void keywordSelected(int64_t keywordId, const QString& fullPath);
    // M9: 点击智能收藏夹项；新建请求由 MainWindow 打开对话框执行 (dialog).
    void smartCollectionSelected(int64_t smartCollectionId);
    void newSmartCollectionRequested();

protected:
    // M9: 右键智能收藏夹分组弹出「新建智能收藏夹」菜单 (context menu).
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void onItemSelected(QTreeWidgetItem* item, int column);
    void onFilterChanged();
    // 重建前记录当前选中项、重建后恢复选中 (restore selection after rebuild).
    void rememberSelection();
    void restoreSelection();
    // 将分组根插入到固定顺序 (folder < library < virtualCopies < collections
    // < smartCollections < keywords) 的正确位置 (stable group order).
    void insertTopLevelInOrder(QTreeWidgetItem* root);
    int groupPriority(const QString& kind) const;

    QComboBox* ratingCombo_ = nullptr;
    QComboBox* flagCombo_ = nullptr;
    QComboBox* labelColorCombo_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* statsLabel_ = nullptr;
    QPushButton* newSmartButton_ = nullptr;
    QTreeWidgetItem* allPhotos_ = nullptr;
    QTreeWidgetItem* folderRoot_ = nullptr;
    QTreeWidgetItem* libraryRoot_ = nullptr;
    QTreeWidgetItem* virtualCopiesRoot_ = nullptr;
    QTreeWidgetItem* keywordsRoot_ = nullptr;
    QTreeWidgetItem* collectionsRoot_ = nullptr;
    QTreeWidgetItem* smartCollectionsRoot_ = nullptr;
    QString libraryRootPath_;
    QString prevKind_;             // 重建前选中的 kind (all/folder/library/...).
    QString prevPath_;             // 选中项存储的路径 (kRolePath) 或空.
    qint64 prevCollectionId_ = -1; // 选中集合的 id，无则 -1.
    // M6/M8/M9 重建前的选中项 (remembered selection for the new kinds).
    qint64 prevImageId_ = -1;
    int prevVariantId_ = -1;
    qint64 prevKeywordId_ = -1;
    qint64 prevSmartCollectionId_ = -1;
};
