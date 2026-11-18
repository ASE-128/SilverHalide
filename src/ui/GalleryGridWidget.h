#pragma once

#include <QImage>
#include <QVector>
#include <QWidget>

class QListView;
class GalleryGridModel;

/**
 * @brief Virtualized icon-mode grid for catalog photos.
 *
 * The widget owns only visible presentation state; file metadata comes from
 * the catalog and thumbnails are requested lazily as rows enter the viewport.
 */
class GalleryGridWidget : public QWidget {
    Q_OBJECT
public:
    struct Entry {
        int imageId = -1;
        QString filePath;
        int rating = 0;
        int flag = 0;
        int labelColor = 0;
        QImage thumbnail;
        // B-2 pregrade marker: 0 = not pregraded, 1 = pregraded without a
        // detected subject, 2 = pregraded with a subject. Mirrors the
        // FilmstripWidget badge semantics; `suggestAuto` adds the amber dot.
        // Appended after `thumbnail` so existing aggregate initializers in
        // MainWindow / Qt tests keep mapping their six leading members.
        int pregradeState = 0;
        bool suggestAuto = false;
    };

    explicit GalleryGridWidget(QWidget* parent = nullptr);

    void setImages(const QVector<Entry>& entries);
    void setThumbnail(int imageId, const QImage& thumbnail);
    void setRating(int imageId, int rating);
    void setFlag(int imageId, int flag);
    void setLabelColor(int imageId, int color);
    void setPregraded(int imageId, bool hasSubject, bool suggestAuto);
    void selectImage(int imageId);
    [[nodiscard]] QVector<int> selectedImageIds() const;
    [[nodiscard]] int currentImageId() const;
    [[nodiscard]] int count() const;

signals:
    void currentImageChanged(int imageId);
    void selectedImagesChanged(const QVector<int>& imageIds);
    void thumbnailRequested(int imageId);
    void bulkRatingRequested(const QVector<int>& imageIds, int rating);
    void bulkFlagRequested(const QVector<int>& imageIds, int flag);
    void bulkLabelColorRequested(const QVector<int>& imageIds, int color);

private:
    void requestVisibleThumbnails();
    void showContextMenu(const QPoint& position);
    void emitSelection();

    QListView* view_ = nullptr;
    GalleryGridModel* model_ = nullptr;
};
