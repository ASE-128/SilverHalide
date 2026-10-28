#include "ui/GalleryGridWidget.h"

#include <algorithm>

#include <QAbstractListModel>
#include <QAction>
#include <QColor>
#include <QFileInfo>
#include <QHash>
#include <QIcon>
#include <QItemSelectionModel>
#include <QMenu>
#include <QPixmap>
#include <QScrollBar>
#include <QTimer>
#include <QListView>
#include <QVBoxLayout>

namespace {

constexpr int kImageIdRole = Qt::UserRole + 1;
constexpr int kFilePathRole = Qt::UserRole + 2;
constexpr int kRatingRole = Qt::UserRole + 3;
constexpr int kFlagRole = Qt::UserRole + 4;
constexpr int kLabelColorRole = Qt::UserRole + 5;
} // namespace

class GalleryGridModel final : public QAbstractListModel {
public:
    explicit GalleryGridModel(QObject* parent = nullptr)
        : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : entries_.size();
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 ||
            index.row() >= entries_.size())
            return {};
        const GalleryGridWidget::Entry& entry = entries_[index.row()];
        switch (role) {
        case Qt::DisplayRole: {
            QString badge;
            if (entry.rating > 0)
                badge += QString(entry.rating, QChar(0x2605));
            if (entry.flag == 1)
                badge += QStringLiteral("  ✓");
            else if (entry.flag == -1)
                badge += QStringLiteral("  ×");
            return badge.isEmpty()
                       ? QFileInfo(entry.filePath).fileName()
                       : badge + QLatin1Char('\n') +
                             QFileInfo(entry.filePath).fileName();
        }
        case Qt::DecorationRole:
            return entry.thumbnail.isNull()
                       ? QVariant{}
                       : QVariant::fromValue(QIcon(
                             QPixmap::fromImage(entry.thumbnail)));
        case Qt::BackgroundRole: {
            static const QColor labelColors[] = {
                QColor(), QColor(190, 55, 55, 55), QColor(220, 125, 35, 55),
                QColor(215, 190, 35, 55), QColor(55, 160, 75, 55),
                QColor(35, 155, 165, 55), QColor(55, 105, 200, 55),
                QColor(145, 75, 180, 55)};
            return entry.labelColor >= 1 && entry.labelColor <= 7
                       ? QVariant(labelColors[entry.labelColor])
                       : QVariant{};
        }
        case Qt::ToolTipRole:
        case kFilePathRole:
            return entry.filePath;
        case kImageIdRole:
            return entry.imageId;
        case kRatingRole:
            return entry.rating;
        case kFlagRole:
            return entry.flag;
        case kLabelColorRole:
            return entry.labelColor;
        default:
            return {};
        }
    }

    void setEntries(const QVector<GalleryGridWidget::Entry>& entries) {
        beginResetModel();
        entries_ = entries;
        rowByImage_.clear();
        for (int i = 0; i < entries_.size(); ++i)
            rowByImage_.insert(entries_[i].imageId, i);
        endResetModel();
    }

    bool setThumbnail(int imageId, const QImage& thumbnail) {
        return updateEntry(imageId, [&thumbnail](GalleryGridWidget::Entry& entry) {
            entry.thumbnail = thumbnail;
        }, {Qt::DecorationRole});
    }

    bool setRating(int imageId, int rating) {
        return updateEntry(imageId, [rating](GalleryGridWidget::Entry& entry) {
            entry.rating = rating;
        }, {Qt::DisplayRole, kRatingRole});
    }

    bool setFlag(int imageId, int flag) {
        return updateEntry(imageId, [flag](GalleryGridWidget::Entry& entry) {
            entry.flag = flag;
        }, {Qt::DisplayRole, kFlagRole});
    }

    bool setLabelColor(int imageId, int color) {
        return updateEntry(imageId, [color](GalleryGridWidget::Entry& entry) {
            entry.labelColor = color;
        }, {Qt::DisplayRole, kLabelColorRole});
    }

    template <class Update>
    bool updateEntry(int imageId, Update&& update, const QList<int>& roles) {
        const auto it = rowByImage_.constFind(imageId);
        if (it == rowByImage_.cend())
            return false;
        update(entries_[it.value()]);
        const QModelIndex changed = index(it.value(), 0);
        emit dataChanged(changed, changed, roles);
        return true;
    }

    int rowForImage(int imageId) const {
        return rowByImage_.value(imageId, -1);
    }

    int imageIdAt(int row) const {
        return row >= 0 && row < entries_.size() ? entries_[row].imageId : -1;
    }

    bool hasThumbnail(int row) const {
        return row >= 0 && row < entries_.size() &&
               !entries_[row].thumbnail.isNull();
    }

private:
    QVector<GalleryGridWidget::Entry> entries_;
    QHash<int, int> rowByImage_;
};

GalleryGridWidget::GalleryGridWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    view_ = new QListView(this);
    model_ = new GalleryGridModel(view_);
    view_->setModel(model_);
    view_->setViewMode(QListView::IconMode);
    view_->setFlow(QListView::LeftToRight);
    view_->setWrapping(true);
    view_->setResizeMode(QListView::Adjust);
    view_->setMovement(QListView::Static);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setSelectionBehavior(QAbstractItemView::SelectItems);
    view_->setIconSize(QSize(160, 112));
    view_->setGridSize(QSize(188, 156));
    view_->setSpacing(8);
    view_->setWordWrap(true);
    view_->setUniformItemSizes(true);
    view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(view_);

    connect(view_->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex& current) {
                emit currentImageChanged(model_->imageIdAt(current.row()));
            });
    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this] { emitSelection(); });
    connect(view_, &QWidget::customContextMenuRequested, this,
            &GalleryGridWidget::showContextMenu);
    connect(view_->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { QTimer::singleShot(0, this, [this] {
                requestVisibleThumbnails();
            }); });
    connect(view_->horizontalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { QTimer::singleShot(0, this, [this] {
                requestVisibleThumbnails();
            }); });
}

void GalleryGridWidget::setImages(const QVector<Entry>& entries) {
    model_->setEntries(entries);
    QTimer::singleShot(0, this, [this] { requestVisibleThumbnails(); });
}

void GalleryGridWidget::setThumbnail(int imageId, const QImage& thumbnail) {
    model_->setThumbnail(imageId, thumbnail);
}

void GalleryGridWidget::setRating(int imageId, int rating) {
    model_->setRating(imageId, rating);
}

void GalleryGridWidget::setFlag(int imageId, int flag) {
    model_->setFlag(imageId, flag);
}

void GalleryGridWidget::setLabelColor(int imageId, int color) {
    model_->setLabelColor(imageId, color);
}

void GalleryGridWidget::selectImage(int imageId) {
    const int row = model_->rowForImage(imageId);
    if (row < 0)
        return;
    const QModelIndex index = model_->index(row, 0);
    view_->selectionModel()->setCurrentIndex(
        index, QItemSelectionModel::NoUpdate);
    if (!view_->selectionModel()->isSelected(index))
        view_->selectionModel()->select(index, QItemSelectionModel::Select |
                                                   QItemSelectionModel::Rows);
    view_->scrollTo(index, QAbstractItemView::EnsureVisible);
}

QVector<int> GalleryGridWidget::selectedImageIds() const {
    QVector<int> ids;
    const QModelIndexList rows = view_->selectionModel()->selectedIndexes();
    ids.reserve(rows.size());
    for (const QModelIndex& index : rows) {
        const int id = model_->imageIdAt(index.row());
        if (id > 0 && !ids.contains(id))
            ids.append(id);
    }
    return ids;
}

int GalleryGridWidget::currentImageId() const {
    return model_->imageIdAt(view_->currentIndex().row());
}

int GalleryGridWidget::count() const {
    return model_->rowCount();
}

void GalleryGridWidget::requestVisibleThumbnails() {
    if (model_->rowCount() == 0)
        return;
    const QRect visible = view_->viewport()->rect();
    QModelIndex first = view_->indexAt(visible.topLeft() + QPoint(2, 2));
    QModelIndex last = view_->indexAt(visible.bottomRight() - QPoint(2, 2));
    if (!first.isValid())
        first = view_->indexAt(QPoint(view_->viewport()->width() / 2, 2));
    if (!last.isValid())
        last = view_->indexAt(QPoint(view_->viewport()->width() / 2,
                                     view_->viewport()->height() - 2));
    if (!first.isValid())
        return;
    const int firstRow = first.row();
    const int lastRow = last.isValid() ? last.row() : firstRow;
    const int columns = std::max(
        1, view_->viewport()->width() / std::max(1, view_->gridSize().width()));
    const int start = std::max(0, firstRow - columns);
    const int finish =
        std::min(model_->rowCount() - 1, lastRow + columns);
    for (int row = start; row <= finish; ++row) {
        if (!model_->hasThumbnail(row)) {
            const int id = model_->imageIdAt(row);
            if (id > 0)
                emit thumbnailRequested(id);
        }
    }
}

void GalleryGridWidget::showContextMenu(const QPoint& position) {
    const QModelIndex clicked = view_->indexAt(position);
    if (!clicked.isValid())
        return;
    if (!view_->selectionModel()->isSelected(clicked)) {
        view_->selectionModel()->clearSelection();
        view_->setCurrentIndex(clicked);
        view_->selectionModel()->select(
            clicked, QItemSelectionModel::Select |
                         QItemSelectionModel::Rows);
    }
    const QVector<int> ids = selectedImageIds();
    if (ids.isEmpty())
        return;

    QMenu menu(this);
    QMenu* rating = menu.addMenu(tr("设为评分"));
    for (int value = 0; value <= 5; ++value) {
        QAction* action = rating->addAction(
            value == 0 ? tr("清除评分") : tr("%1 星").arg(value));
        connect(action, &QAction::triggered, this,
                [this, ids, value] { emit bulkRatingRequested(ids, value); });
    }
    QMenu* flag = menu.addMenu(tr("设为旗标"));
    const struct {
        const char* text;
        int value;
    } flags[] = {{"已选取", 1}, {"已拒绝", -1}, {"无旗标", 0}};
    for (const auto& option : flags) {
        QAction* action = flag->addAction(tr(option.text));
        connect(action, &QAction::triggered, this, [this, ids, value = option.value] {
            emit bulkFlagRequested(ids, value);
        });
    }
    QMenu* color = menu.addMenu(tr("设为色标"));
    const char* colors[] = {"清除", "红", "橙", "黄", "绿", "青", "蓝", "紫"};
    for (int value = 0; value <= 7; ++value) {
        QAction* action = color->addAction(tr(colors[value]));
        connect(action, &QAction::triggered, this,
                [this, ids, value] {
                    emit bulkLabelColorRequested(ids, value);
                });
    }
    menu.exec(view_->viewport()->mapToGlobal(position));
}

void GalleryGridWidget::emitSelection() {
    emit selectedImagesChanged(selectedImageIds());
}
