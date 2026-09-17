#pragma once

/**
 * @brief Duplicate-results modal.
 *
 * Renders the output of DuplicateDetector (a vector of groups, each group
 * being a vector of photo IDs + mean similarity) as a two-pane view:
 *   * left:  group list (group index + member count + mean similarity)
 *   * right: the member images of the selected group, side-by-side, with each
 *            row showing photo id, file path (looked up via the
 *            CatalogDatabase), and similarity info
 *
 * Below the images is an action bar:
 *   * Stack into single stack (uses CatalogDatabase::setStackId / nextStackId
 *     to create a new stack for the selected group)
 *   * Focus members in the filmstrip/grid
 *   * Dismiss without modifying the catalog
 *
 * The caller is responsible for ensuring DuplicateResultsDialog is destroyed
 * before the CatalogDatabase it references (this matches the existing
 * ownership: MainWindow owns both).
 */

#include <QDialog>
#include <QVector>

#include "app/DuplicateDetector.h"

class QListWidget;
class QTableWidget;
class QLabel;
class QPushButton;

namespace lrc {

class CatalogDatabase;

class DuplicateResultsDialog : public QDialog {
    Q_OBJECT
public:
    // Accepts the DuplicateResult groups directly (std::vector → copied into
    // a QVector for convenient Qt index access).  `db` may be null (in which
    // case stacking is disabled and only IDs are shown).
    DuplicateResultsDialog(const std::vector<DuplicateGroup>& groups,
                           CatalogDatabase* db,
                           QWidget* parent = nullptr);

signals:
    // Emitted *after* CatalogDatabase has committed the new stack IDs.
    // The caller (MainWindow) is expected to refresh its grid view.
    void groupStacked(int64_t stackId);
    // Emitted when the user asks to focus (jump to) one or more image ids in
    // the filmstrip / grid — MainWindow selects those rows.
    void focusImagesRequested(const QVector<int64_t>& imageIds);

private slots:
    void onGroupSelectionChanged();
    void onStackClicked();
    void onFocusClicked();

private:
    void rebuildTableForGroup(int groupIndex);
    QString describeGroup(int index, const DuplicateGroup& group) const;

    QVector<DuplicateGroup> groups_;
    CatalogDatabase* db_ = nullptr;

    QListWidget* groupList_ = nullptr;
    QTableWidget* membersTable_ = nullptr;
    QLabel* groupSummaryLabel_ = nullptr;
    QPushButton* stackBtn_ = nullptr;
    QPushButton* focusBtn_ = nullptr;
    QPushButton* closeBtn_ = nullptr;
};

} // namespace lrc
