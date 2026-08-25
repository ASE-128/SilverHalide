#pragma once

// Smart-collections (rule-based dynamic collections) management dialog:
// list / create / edit / delete smart collections with a rule builder
// (field + operator + value rows and an ALL/ANY match selector). Operates
// directly on the injected catalog and emits smartCollectionsChanged() after
// any mutation so MainWindow can refresh its navigator and filmstrip
// (mirrors CollectionsDialog, PHASE_9).

#include <cstdint>
#include <vector>

#include <QDialog>

#include "catalog/CatalogDatabase.h"

class QComboBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QVBoxLayout;
class QWidget;

class RuleRowWidget;

class SmartCollectionsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SmartCollectionsDialog(lrc::CatalogDatabase* catalog,
                                    QWidget* parent = nullptr);

signals:
    void smartCollectionsChanged();

private slots:
    void onSelectionChanged();
    void onNewCollection();
    void onEditCollection();
    void onDeleteCollection();
    void onSave();
    void onAddRule();
    void onRemoveRule();

private:
    void refreshList();
    void clearEditor();
    void loadCollection(const lrc::SmartCollection& sc);
    void removeRule(RuleRowWidget* row);
    int64_t currentCollectionId() const;
    QString collectionName(int64_t id) const;
    QString buildPredicateJson() const;
    bool hasValidRules() const;

    lrc::CatalogDatabase* catalog_ = nullptr;
    QListWidget* list_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QComboBox* matchCombo_ = nullptr;
    QVBoxLayout* rulesLayout_ = nullptr;
    QWidget* rulesContainer_ = nullptr;
    QPushButton* editButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    std::vector<RuleRowWidget*> rules_;
    int64_t editingId_ = -1;  // -1 = new collection
};
