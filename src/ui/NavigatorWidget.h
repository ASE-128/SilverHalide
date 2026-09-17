#pragma once

/**
 * @brief Left navigation panel.
 *
 * Shows "All Photos", the imported folders, rating / flag / label-colour
 * filters, a "Collections" group, a library tree, a virtual-copies group, a
 * keyword-list group, a smart-collections group, and a small statistics
 * footer. Selecting an entry emits a signal that filters the filmstrip. Lives
 * on the UI thread; it never touches the catalog directly, only through
 * signals.
 */

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
    // Renders the managed library root and its sub-folders (relative paths, e.g.
    // "2026/2026-08-11"). `libraryRoot` is the absolute library-root path;
    // `subfolders` is a sorted, de-duplicated list of paths relative to it. An
    // empty list means an empty library; an empty `libraryRoot` clears the whole
    // library group.
    void setLibrary(const QString& libraryRoot, const QStringList& subfolders);
    // Renders the "Collections" group; each child is a collection. An empty list
    // keeps only a placeholder root item.
    void setCollections(const QVector<lrc::Collection>& collections);
    void setStatistics(int imageCount, int ratedCount);

    // Fills the "Virtual copies" group. Each item is (imageId, variant); shows
    // variant.name ("Master" when null). MainWindow may pre-fill the name to
    // distinguish different images.
    void setVirtualCopies(
        const std::vector<std::pair<int64_t, lrc::EditVariant>>& virtualCopies);
    // Fills the "Keywords" group, showing full paths with photo counts (e.g.
    // "landscape/Iceland/waterfall (42)").
    void setKeywords(const std::vector<lrc::KeywordInfo>& keywords);
    // Fills the "Smart collections" group; an empty list keeps only a
    // placeholder root item.
    void setSmartCollections(
        const std::vector<lrc::SmartCollection>& smartCollections);

signals:
    void allPhotosSelected();
    void folderSelected(const QString& folderPath);
    // Rating filter 0..5 (0 = any). Flag filter: 0 = any, 1 = picked,
    // -1 = rejected, 2 = no flag. Label-colour filter: 0 = any, 1..7.
    void filtersChanged(int minRating, int flagFilter, int labelColorFilter);
    void collectionSelected(int collectionId);
    // Clicked a virtual-copy item (imageId, variantId).
    void virtualCopySelected(int64_t imageId, int variantId);
    // Clicked a keyword item (keywordId, full path).
    void keywordSelected(int64_t keywordId, const QString& fullPath);
    // Clicked a smart-collection item; a create request opens the dialog in
    // MainWindow.
    void smartCollectionSelected(int64_t smartCollectionId);
    void newSmartCollectionRequested();

protected:
    // Right-clicking the smart-collections group pops a "New smart
    // collection" menu.
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void onItemSelected(QTreeWidgetItem* item, int column);
    void onFilterChanged();
    // Remembers the current selection before a rebuild and restores it after.
    void rememberSelection();
    void restoreSelection();
    // Inserts a group root at the correct position in the fixed group order
    // (folder < library < virtualCopies < collections < smartCollections
    // < keywords), keeping the groups stable across rebuilds.
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
    QString prevKind_;             // kind selected before rebuild (all/folder/library/...).
    QString prevPath_;             // stored path of the selected item (kRolePath), or empty.
    qint64 prevCollectionId_ = -1; // id of the selected collection, or -1.
    // Remembered selection for the new kinds before a rebuild.
    qint64 prevImageId_ = -1;
    int prevVariantId_ = -1;
    qint64 prevKeywordId_ = -1;
    qint64 prevSmartCollectionId_ = -1;
};
