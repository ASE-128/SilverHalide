#include "ui/DuplicateResultsDialog.h"

#include <QHeaderView>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "catalog/CatalogDatabase.h"

namespace lrc {

DuplicateResultsDialog::DuplicateResultsDialog(
    const std::vector<DuplicateGroup>& groups,
    CatalogDatabase* db,
    QWidget* parent)
    : QDialog(parent), db_(db) {
    // Convert std::vector → QVector for Qt-style index access.
    groups_.reserve(static_cast<int>(groups.size()));
    for (const auto& g : groups) groups_.push_back(g);

    setWindowTitle(tr("重复项检测结果"));
    resize(920, 600);

    auto* root = new QVBoxLayout(this);

    groupSummaryLabel_ = new QLabel(this);
    groupSummaryLabel_->setTextFormat(Qt::RichText);
    groupSummaryLabel_->setWordWrap(true);
    root->addWidget(groupSummaryLabel_);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    root->addWidget(splitter, 1);

    groupList_ = new QListWidget(this);
    groupList_->setSelectionMode(QAbstractItemView::SingleSelection);
    splitter->addWidget(groupList_);

    membersTable_ = new QTableWidget(0, 4, this);
    membersTable_->setHorizontalHeaderLabels(
        QStringList{tr("照片 ID"), tr("文件路径"), tr("相似度"), tr("备注")});
    membersTable_->horizontalHeader()->setStretchLastSection(true);
    membersTable_->verticalHeader()->setVisible(false);
    membersTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    membersTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    splitter->addWidget(membersTable_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);

    auto* btnBox = new QHBoxLayout();
    stackBtn_ = new QPushButton(tr("堆叠所选组"), this);
    focusBtn_ = new QPushButton(tr("在网格中定位成员"), this);
    closeBtn_ = new QPushButton(tr("关闭"), this);
    btnBox->addWidget(stackBtn_);
    btnBox->addWidget(focusBtn_);
    btnBox->addStretch(1);
    btnBox->addWidget(closeBtn_);
    root->addLayout(btnBox);

    // Populate the group list.
    for (int i = 0; i < groups_.size(); ++i) {
        auto* item = new QListWidgetItem(describeGroup(i, groups_[i]), groupList_);
        item->setData(Qt::UserRole, i);
    }

    int totalImages = 0;
    for (const auto& g : groups_) totalImages += static_cast<int>(g.memberIds.size());

    groupSummaryLabel_->setText(
        tr("共检测到 <b>%1 组重复项</b>，涉及 %2 张图片。"
           "在左侧选择一组，然后执行堆叠或定位。").arg(groups_.size())
            .arg(totalImages));

    connect(groupList_, &QListWidget::currentRowChanged, this,
            &DuplicateResultsDialog::onGroupSelectionChanged);
    connect(stackBtn_, &QPushButton::clicked, this,
            &DuplicateResultsDialog::onStackClicked);
    connect(focusBtn_, &QPushButton::clicked, this,
            &DuplicateResultsDialog::onFocusClicked);
    connect(closeBtn_, &QPushButton::clicked, this, &QDialog::accept);

    if (!groups_.isEmpty()) {
        groupList_->setCurrentRow(0);
    }
}

QString DuplicateResultsDialog::describeGroup(int index,
                                              const DuplicateGroup& g) const {
    // Scannable one-liner: group number, member count, mean cosine sim.
    return tr("组 %1 — %2 张 — 平均相似度 %3")
        .arg(index + 1)
        .arg(g.memberIds.size())
        .arg(static_cast<double>(g.meanSimilarity), 0, 'f', 3);
}

void DuplicateResultsDialog::onGroupSelectionChanged() {
    const QListWidgetItem* cur = groupList_->currentItem();
    if (!cur) {
        rebuildTableForGroup(-1);
        return;
    }
    bool ok = false;
    const int idx = cur->data(Qt::UserRole).toInt(&ok);
    rebuildTableForGroup(ok ? idx : -1);
}

void DuplicateResultsDialog::rebuildTableForGroup(int groupIndex) {
    membersTable_->setRowCount(0);
    if (groupIndex < 0 || groupIndex >= groups_.size())
        return;
    const auto& g = groups_[groupIndex];
    const int n = static_cast<int>(g.memberIds.size());
    membersTable_->setRowCount(n);
    for (int r = 0; r < n; ++r) {
        const int64_t photoId = g.memberIds[r];

        auto* idItem = new QTableWidgetItem(QString::number(photoId));

        QString pathText;
        if (db_) {
            if (auto rec = db_->findImageById(photoId)) {
                pathText = QString::fromStdString(rec->filePath);
            } else {
                pathText = tr("（目录库中未找到）");
            }
        } else {
            pathText = tr("（无目录库连接）");
        }
        auto* pathItem = new QTableWidgetItem(pathText);

        // Mean similarity is a per-group value; we repeat it per row so the
        // right-hand table is self-contained (users looking at a single
        // group don't need to cross-reference the left list).
        auto* simItem = new QTableWidgetItem(
            QString::number(static_cast<double>(g.meanSimilarity), 'f', 4));

        QString notes;
        if (r == 0) {
            notes += tr("（代表）");
        }
        auto* notesItem = new QTableWidgetItem(notes);

        membersTable_->setItem(r, 0, idItem);
        membersTable_->setItem(r, 1, pathItem);
        membersTable_->setItem(r, 2, simItem);
        membersTable_->setItem(r, 3, notesItem);
    }
    membersTable_->resizeColumnsToContents();
}

void DuplicateResultsDialog::onStackClicked() {
    if (!db_) {
        QMessageBox::warning(this, tr("堆叠"),
                             tr("没有可用的目录库连接。"));
        return;
    }
    const QListWidgetItem* cur = groupList_->currentItem();
    if (!cur) return;
    bool ok = false;
    const int idx = cur->data(Qt::UserRole).toInt(&ok);
    if (!ok || idx < 0 || idx >= groups_.size()) return;
    const auto& g = groups_[idx];
    if (g.memberIds.size() < 2) {
        QMessageBox::information(this, tr("堆叠"),
                                 tr("至少需要 2 张图片才能组成堆叠。"));
        return;
    }
    const int64_t stackId = db_->nextStackId();
    bool allOk = true;
    for (int64_t photoId : g.memberIds) {
        if (!db_->setStackId(photoId, stackId)) {
            allOk = false;
            break;
        }
    }
    if (!allOk) {
        const QString err = QString::fromStdString(db_->lastError());
        QMessageBox::warning(this, tr("堆叠"),
                             tr("无法提交堆叠 ID。目录库错误：%1")
                                 .arg(err.isEmpty() ? tr("未知") : err));
        return;
    }
    QMessageBox::information(this, tr("堆叠"),
                             tr("已将 %1 张图片堆叠为堆叠 #%2。")
                                 .arg(static_cast<int>(g.memberIds.size()))
                                 .arg(stackId));
    emit groupStacked(stackId);
}

void DuplicateResultsDialog::onFocusClicked() {
    QVector<int64_t> ids;

    // First, collect explicitly-selected rows in the members table.
    const auto selected = membersTable_->selectionModel()->selectedRows();
    const auto* cur = groupList_->currentItem();
    bool ok = false;
    int idx = -1;
    if (cur) idx = cur->data(Qt::UserRole).toInt(&ok);
    if (!ok) idx = -1;

    for (const auto& sel : selected) {
        const int r = sel.row();
        if (r < 0 || idx < 0 || idx >= groups_.size()) continue;
        if (r < static_cast<int>(groups_[idx].memberIds.size()))
            ids.push_back(groups_[idx].memberIds[r]);
    }

    // No explicit selection → focus every member of the currently-selected group.
    if (ids.isEmpty() && idx >= 0 && idx < groups_.size()) {
        for (int64_t photoId : groups_[idx].memberIds)
            ids.push_back(photoId);
    }

    if (!ids.isEmpty()) emit focusImagesRequested(ids);
}

} // namespace lrc
