#pragma once

/**
 * @brief Bottom filmstrip: a horizontal icon-mode list of imported images.
 *
 * The current selection is highlighted; thumbnails stream in from the import /
 * thumbnail worker as they are generated (setThumbnail). Each thumbnail shows
 * Lightroom-style badges: star rating (bottom-left), flag (top-right), colour
 * label (bottom-right), a small collection indicator (top-left) and, for
 * virtual copies, a page-fold copy badge below the colour label carrying the
 * variant number. Rating / flag / colour label / collection membership /
 * removal are driven by keyboard shortcuts and a right-click context menu, then
 * emitted to the catalog layer. Lives on the UI thread.
 *
 * Several entries can share one imageId — a master (variantId 0) and its virtual
 * copies (variantId > 0) all point at the same physical image and thumbnail, so
 * the widget keys its state by imageId and lets one thumbnail feed every
 * affected row.
 */

#include <QHash>
#include <QImage>
#include <QList>
#include <QSet>
#include <QVector>
#include <QWidget>

#include <cstdint>
#include <string>

#include "catalog/CatalogDatabase.h"

class QContextMenuEvent;
class QEvent;
class QKeyEvent;
class QListWidget;

class FilmstripWidget : public QWidget {
    Q_OBJECT
public:
    struct Entry {
        int imageId = 0;
        QString filePath;
        QString thumbnailPath;
        QImage thumbnail;  // embedded raw thumbnail, empty = not loaded.
        int rating = 0;        // 0..5
        int flag = lrc::ImageFlag::FlagNone;
        int labelColor = 0;   // 0 = none, 1..7 (0 colourless, 1..7 colour label).
        QVector<int> collectionIds;  // collection ids this image belongs to.
        // 0 = master (no badge), > 0 = a virtual copy sharing the same physical
        // image / thumbnail with an independent edit stack.
        int variantId = 0;
        // Display name of the virtual copy ("Copy 1", "High-contrast B&W", ...),
        // shown under the thumbnail next to the file name. Empty for the master.
        std::string variantName;
    };

    explicit FilmstripWidget(QWidget* parent = nullptr);

    void setImages(const QVector<Entry>& entries);
    void setThumbnail(int imageId, const QImage& thumb);
    void setRating(int imageId, int rating);
    void setFlag(int imageId, int flag);
    void setLabelColor(int imageId, int color);
    /**
     * @brief Records a B-2 pregrade marker for `imageId` and repaints every row
     *        sharing it (a master and its virtual copies share one id).
     *
     * @param imageId     Catalog id of the analysed image.
     * @param hasSubject  Whether strong foreground saliency was detected; colours
     *                    the badge.
     * @param score       Quality score in [0,100]; a value < 0 clears the badge.
     * @param suggestAuto Whether the tonal statistics recommend running B-1
     *                    auto-develop; draws a distinct "建议自动" dot on the badge.
     *
     * @note This map survives setImages(), so the marker stays visible across
     *       filmstrip rebuilds once recorded (from the import-time live signal or
     *       a catalog backfill).
     */
    void setPregraded(int imageId, bool hasSubject, int score, bool suggestAuto);
    void setInCollection(int imageId, bool in);
    void setCollections(const QVector<lrc::Collection>& collections);
    void selectImage(int imageId);
    // Selects the row carrying `variantId` of `imageId` (0 = master). Falls
    // back to the master row when the exact variant is not present.
    void selectVariant(int imageId, int variantId);
    int currentImageId() const;
    // Variant id of the current row (0 = master, 0 when no row selected).
    int currentVariantId() const;
    int count() const;

signals:
    void selectionChanged(int imageId);
    // Emitted on every selection change together with selectionChanged,
    // carrying the clicked entry's variant id (0 = master). Added alongside
    // selectionChanged so MainWindow's existing master-only path keeps working
    // unchanged; the caller decides which signal to connect (use this one to
    // load the matching variant edit stack).
    void imageVariantSelected(int64_t imageId, int variantId);
    void ratingRequested(int imageId, int rating);
    void flagRequested(int imageId, int flag);
    void labelColorRequested(int imageId, int color);
    void collectionRequested(int imageId, int collectionId, bool add);
    void removeRequested(int imageId);
    // Emitted when a filmstrip item's thumbnail is needed but not yet loaded.
    // MainWindow connects this, decodes off the main thread and calls
    // setThumbnail(imageId, thumb) to back-fill the row.
    void thumbnailRequested(int imageId);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void onCurrentChanged(int row);
    int imageIdAt(int row) const;
    Entry* entryAt(int row);
    const Entry* entryAt(int row) const;
    // All rows whose entry carries `imageId` (a master and its virtual copies
    // share the imageId, so several rows can map to one imageId). Used by the
    // thumbnail/rating/flag/label/collection setters to refresh every affected
    // row instead of only the single row an imageId->row map could name.
    QVector<int> rowsForImage(int imageId) const;
    void refreshIcon(int row);
    // B-2 pregrade badge state for `imageId`: 0 = none, 1 = pregraded without a
    // subject, 2 = pregraded with a detected subject.
    int pregradeState(int imageId) const;

    // Lazy loading: recompute the visible window (current viewport + one screen
    // of look-ahead/look-behind) and request thumbnails for any item in it that
    // is not yet loaded and not already in flight.
    void updateVisibleRange();
    void requestThumbnail(int imageId);
    bool hasRawThumb(int imageId) const;
    // Returns the stored raw thumbnail, touching the LRU on a hit and falling
    // back to the QPixmapCache raw cache after an LRU eviction.
    QImage rawThumbnail(int imageId);
    void storeRawThumbnail(int imageId, const QImage& thumb);
    void touchLru(int imageId);

    QListWidget* list_ = nullptr;
    QVector<Entry> entries_;
    QVector<lrc::Collection> collections_;
    // imageId -> raw (un-badged) thumbnail used as the base for badge
    // composition in refreshIcon. Bounded to kRawThumbsMax entries by the LRU
    // order in lruOrder_ (front = least recently used); evicted entries stay
    // recoverable from QPixmapCache under a filmstrip_raw_* key.
    QHash<int, QImage> rawThumbs_;
    // imageId -> thumbnail version; bumped on every setThumbnail so the
    // badge-cache key changes when the base image changes (invalidation).
    QHash<int, int> thumbVersions_;
    // LRU recency order for rawThumbs_: front = least recently used, back =
    // most recently used. Kept in sync with rawThumbs_.
    QList<int> lruOrder_;
    // imageIds with an outstanding thumbnailRequested emission (dedupe: a given
    // imageId is never requested twice before its back-fill arrives).
    QSet<int> pendingThumbRequests_;
    // B-2 pregrade markers, keyed by imageId (a master and its virtual copies
    // share the id). Kept separate from Entry so they survive setImages().
    // score >= 0 signals a marker is present; hasSubject colours the badge.
    // suggestAuto draws a distinct "建议自动" dot on the badge.
    QHash<int, bool> pregradeSubjects_;
    QHash<int, int> pregradeScores_;
    QHash<int, bool> pregradeAuto_;
};
