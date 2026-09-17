#pragma once

/**
 * @brief Collections management dialog.
 *
 * Create / rename / delete manual collections and manage their member photos.
 * Operates directly on the injected catalog (data driven); MainWindow
 * instantiates it from a menu and refreshes its navigator whenever
 * collectionsChanged() fires.
 */

#include <cstdint>

#include <QDialog>

#include "catalog/CatalogDatabase.h"

class QLabel;
class QListWidget;
class QPushButton;

class CollectionsDialog : public QDialog {
    Q_OBJECT
public:
    explicit CollectionsDialog(lrc::CatalogDatabase* catalog, QWidget* parent = nullptr);

signals:
    void collectionsChanged();

private slots:
    void onSelectionChanged();
    void onCreateCollection();
    void onRenameCollection();
    void onDeleteCollection();
    void onAddPhotos();
    void onRemoveSelected();

private:
    void refreshCollections();
    void refreshMembers();
    int64_t currentCollectionId() const;
    QString collectionName(int64_t id) const;

    lrc::CatalogDatabase* catalog_ = nullptr;
    QListWidget* collectionsList_ = nullptr;
    QListWidget* membersList_ = nullptr;
    QLabel* collectionTitle_ = nullptr;
    QPushButton* renameButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;
};
