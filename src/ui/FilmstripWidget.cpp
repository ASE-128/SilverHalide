#include "ui/FilmstripWidget.h"

#include <QContextMenuEvent>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QKeyEvent>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPixmapCache>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int kIconWidth = 144;
constexpr int kIconHeight = 100;
constexpr int kItemRole = Qt::UserRole;

// Grid stride used by setGridSize() in the constructor; the visible-window
// algorithm walks the scrollbar in these units because the list lays items out
// in a wrapping grid of exactly this cell size (uniform item sizes).
constexpr int kGridW = kIconWidth + 12;   // matches setGridSize width.
constexpr int kGridH = kIconHeight + 26;  // matches setGridSize height.

// Raw-thumbnail LRU bound: keep at most this many base images in rawThumbs_ so
// the heap does not grow with the scroll position. Evicted entries fall back to
// QPixmapCache (filmstrip_raw_* keys) and are re-seeded on demand.
constexpr int kRawThumbsMax = 128;

// Sentinel collection id used by setInCollection(true): the setter carries only
// a boolean, and the badge needs no real id -- a full refresh restores the real
// collectionIds list later.
constexpr int kCollectionSentinel = -1;

// Virtual-copy badge geometry: a light "page" with a dog-eared
// bottom-right corner, drawn at the thumbnail's bottom-right corner. When a
// colour label is present on the same tile the label square is lifted above it
// (see makeBadgedIcon) so the two bottom-right badges never overlap.
constexpr int kCopyBadgeSize = 18;  // page width / height in px.
constexpr int kCopyBadgeGap = 3;    // badge inset from the tile corner in px.
constexpr int kCopyFoldLeg = 4;     // dog-ear triangle leg length in px.

// Badge-composite cache bound: keep at most this many filmstrip tiles in
// QPixmapCache before dropping the cache entirely (simple count-based eviction;
// QPixmapCache itself has only a global byte limit we cannot steer per-key).
constexpr int kBadgeCacheMax = 64;

// Number of badge pixmaps inserted since the last clear. QPixmapCache is
// process-global, so a single counter shared across all FilmstripWidget
// instances is correct (the cache is global too).
int g_badgeCacheInserts = 0;

// The cache key covers every input that changes the composite appearance:
// imageId, thumbnail version (rotated by setThumbnail so a changed base image
// never reuses a stale composite), rating, flag, colour label, collection
// membership, the B-2 pregrade state and -- for virtual copies -- the variant
// id. A virtual copy shares the imageId of its master, so without variantId the
// two tiles would collide in the cache. The "filmstrip_badge_" prefix keeps
// these keys from colliding with any other QPixmapCache user in the process.
QString badgeCacheKey(int imageId, int thumbVersion, int rating, int flag,
                      int labelColor, bool inCollection, int pregradeState,
                      int variantId, bool suggestAuto) {
    return QStringLiteral("filmstrip_badge_%1_%2_%3_%4_%5_%6_%7_%8_%9")
        .arg(imageId)
        .arg(thumbVersion)
        .arg(rating)
        .arg(flag)
        .arg(labelColor)
        .arg(inCollection ? 1 : 0)
        .arg(pregradeState)
        .arg(variantId)
        .arg(suggestAuto ? 1 : 0);
}

void clearBadgeCache() {
    QPixmapCache::clear();
    g_badgeCacheInserts = 0;
}

void insertBadgePixmap(const QString& key, const QPixmap& pm) {
    QPixmapCache::insert(key, pm);
    // Once more than kBadgeCacheMax tiles have been inserted since the last
    // clear, drop everything. Filmstrip composites are cheap to regenerate, so
    // a blunt clear keeps the global cache from being dominated by them (ends
    // with roughly the newest kBadgeCacheMax tiles resident).
    if (++g_badgeCacheInserts > kBadgeCacheMax)
        clearBadgeCache();
}

// Raw-thumbnail QPixmapCache key. The thumbnail version is part of the key so
// an updated base image (setThumbnail) never reuses a stale fallback.
QString rawCacheKey(int imageId, int version) {
    return QStringLiteral("filmstrip_raw_%1_%2").arg(imageId).arg(version);
}

// Lightroom colour-label palette, indexed 1..7 (0 = none).
QColor labelColorRgb(int labelColor) {
    switch (labelColor) {
        case 1: return QColor(230, 80, 80);   // red
        case 2: return QColor(235, 150, 60);  // orange
        case 3: return QColor(230, 210, 60);  // yellow
        case 4: return QColor(110, 200, 90);  // green
        case 5: return QColor(80, 190, 190);  // cyan
        case 6: return QColor(80, 140, 230);  // blue
        case 7: return QColor(170, 100, 210); // purple
        default: return QColor();
    }
}

// Display names only; the literal is translated with tr() where the context menu
// is built, so QT_TR_NOOP is what makes it visible to the translation tooling.
const char* kLabelColorNames[7] = {
    QT_TR_NOOP("红"), QT_TR_NOOP("橙"), QT_TR_NOOP("黄"), QT_TR_NOOP("绿"),
    QT_TR_NOOP("青"), QT_TR_NOOP("蓝"), QT_TR_NOOP("紫")};

// Draws the virtual-copy badge: a light "page" with a dog-eared
// (folded-back) bottom-right corner, carrying the variant number. It sits at
// the thumbnail's bottom-right corner; makeBadgedIcon lifts the colour-label
// square above it so the two bottom-right badges never overlap.
void drawVariantBadge(QPainter& p, int variantId) {
    const QRect page(kIconWidth - kCopyBadgeSize - kCopyBadgeGap,
                     kIconHeight - kCopyBadgeSize - kCopyBadgeGap,
                     kCopyBadgeSize, kCopyBadgeSize);
    const QPoint corner(page.right(), page.bottom());
    // Folded-back triangle (the dog-ear) at the page's bottom-right corner.
    QPainterPath fold;
    fold.moveTo(corner);
    fold.lineTo(corner.x() - kCopyFoldLeg, corner.y());
    fold.lineTo(corner.x(), corner.y() - kCopyFoldLeg);
    fold.closeSubpath();
    p.fillRect(page, QColor(235, 235, 240));   // page face.
    p.fillPath(fold, QColor(155, 155, 165));   // folded underside shade.
    p.setPen(QColor(105, 105, 115));
    p.drawRect(page);                          // page outline.
    p.drawLine(corner.x() - kCopyFoldLeg, corner.y(), corner.x(),
               corner.y() - kCopyFoldLeg);     // fold diagonal.
    // Variant number, kept clear of the folded corner.
    const QString num = QString::number(variantId);
    const QRect numRect = page.adjusted(1, 1, -kCopyFoldLeg - 1, -kCopyFoldLeg - 1);
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(num.size() <= 1 ? 10 : (num.size() == 2 ? 8 : 6));
    p.setFont(f);
    p.setPen(QColor(40, 40, 45));
    p.drawText(numRect, Qt::AlignCenter, num);
}

// Composes the thumbnail with LR-style badges: star rating at bottom-left, a
// flag marker at top-right, a colour-label square at bottom-right, a small
// collection indicator at top-left and -- for virtual copies (variantId != 0) --
// a page-fold copy badge at the bottom-right carrying the variant number. When
// a B-2 pregrade marker is present (pregradeState != 0) a small "AI" tag is
// drawn at the bottom-centre; cyan when a subject was detected, grey otherwise.
// Falls back to a dark placeholder when the thumbnail is not available yet.
// Returns the finished tile as a QPixmap so the caller can wrap it in a QIcon
// and/or hand it to QPixmapCache.
QPixmap makeBadgedIcon(const QImage& thumb, int rating, int flag,
                       int labelColor, bool inCollection, int pregradeState,
                       bool suggestAuto, int variantId) {
    QPixmap pm;
    if (thumb.isNull()) {
        pm = QPixmap(kIconWidth, kIconHeight);
        pm.fill(QColor(0x38, 0x38, 0x38));
    } else {
        pm = QPixmap::fromImage(
            thumb.scaled(kIconWidth, kIconHeight, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation));
        // Center the scaled image on the icon tile.
        QPixmap tile(kIconWidth, kIconHeight);
        tile.fill(QColor(0x2b, 0x2b, 0x2b));
        QPainter tp(&tile);
        tp.drawPixmap((kIconWidth - pm.width()) / 2,
                      (kIconHeight - pm.height()) / 2, pm);
        pm = tile;
    }

    QPainter p(&pm);
    p.setRenderHint(QPainter::TextAntialiasing);
    QFont starFont = p.font();
    starFont.setPixelSize(11);
    starFont.setBold(true);
    p.setFont(starFont);

    // Rating stars.
    if (rating > 0) {
        QString stars;
        for (int i = 0; i < rating; ++i)
            stars += QChar(0x2605); // ★
        p.setPen(QColor(255, 205, 40));
        p.drawText(pm.rect().adjusted(3, 0, -3, -2), Qt::AlignLeft | Qt::AlignBottom, stars);
    }

    // Flag marker.
    if (flag == lrc::ImageFlag::FlagPicked) {
        p.setPen(QColor(120, 200, 120));
        p.drawText(pm.rect().adjusted(-3, 2, -3, 0), Qt::AlignRight | Qt::AlignTop,
                   QStringLiteral("P"));
    } else if (flag == lrc::ImageFlag::FlagRejected) {
        p.setPen(QColor(230, 90, 90));
        p.drawText(pm.rect().adjusted(-3, 2, -3, 0), Qt::AlignRight | Qt::AlignTop,
                   QStringLiteral("X"));
    }

    // Colour-label square (bottom-right). For a virtual copy the square is
    // lifted directly above the page-fold badge so the two bottom-right badges
    // never overlap; the master (variantId == 0) keeps its original corner
    // position, so existing master tiles are pixel-identical to before.
    if (labelColor >= 1 && labelColor <= 7) {
        const int y =
            (variantId != 0)
                ? kIconHeight - kCopyBadgeSize - kCopyBadgeGap - 1 - 12
                : kIconHeight - 15;
        const QRect r(kIconWidth - 15, y, 12, 12);
        p.fillRect(r, labelColorRgb(labelColor));
        p.setPen(QColor(220, 220, 220));
        p.drawRect(r);
    }

    // Virtual-copy page-fold badge (bottom-right, directly below the colour
    // label when one is present).
    if (variantId != 0)
        drawVariantBadge(p, variantId);

    // Collection indicator (top-left).
    if (inCollection) {
        const QRect r(3, 3, 10, 10);
        p.fillRect(r, QColor(90, 150, 220));
        p.setPen(QColor(235, 235, 235));
        p.drawRect(r);
    }

    // B-2 pregrade "AI" tag (bottom-centre). Cyan for a detected subject,
    // muted grey otherwise; kept clear of the rating (bottom-left) and the
    // colour-label / copy badge (bottom-right).
    if (pregradeState != 0) {
        QFont aiFont = p.font();
        aiFont.setPixelSize(9);
        aiFont.setBold(true);
        p.setFont(aiFont);
        const bool hasSubject = (pregradeState == 2);
        const QRect tag(kIconWidth - 34, kIconHeight - 16, 30, 12);
        p.setPen(Qt::NoPen);
        p.setBrush(hasSubject ? QColor(30, 190, 200) : QColor(120, 125, 132));
        p.drawRoundedRect(tag, 3, 3);
        p.setPen(QColor(250, 250, 250));
        p.drawText(tag, Qt::AlignCenter, QStringLiteral("AI"));
    }
    // B-1 "建议自动" dot (top-centre): the pregrade statistics recommend running
    // auto-develop on this photo. Kept clear of the collection square (top-left),
    // the flag marker (top-right) and the "AI" tag (bottom-centre).
    if (suggestAuto) {
        const QRect dot(kIconWidth / 2 - 4, 4, 8, 8);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(245, 180, 40));  // amber
        p.drawEllipse(dot);
    }
    return pm;
}

} // namespace

FilmstripWidget::FilmstripWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    list_ = new QListWidget(this);
    list_->setViewMode(QListView::IconMode);
    list_->setFlow(QListView::LeftToRight);
    list_->setWrapping(true);
    list_->setResizeMode(QListView::Adjust);
    list_->setMovement(QListView::Static);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setUniformItemSizes(true);
    list_->setIconSize(QSize(kIconWidth, kIconHeight));
    list_->setSpacing(8);
    list_->setGridSize(QSize(kIconWidth + 12, kIconHeight + 26));
    list_->setWordWrap(true);
    layout->addWidget(list_);

    connect(list_, &QListWidget::currentRowChanged, this,
            &FilmstripWidget::onCurrentChanged);

    // Scroll / layout changes recompute the visible window and request thumbnails
    // for newly visible items (dedupe happens in requestThumbnail). The viewport
    // resize path is handled by eventFilter (deferred one event
    // loop turn so the new layout is applied first).
    connect(list_->verticalScrollBar(), &QScrollBar::valueChanged, this,
            &FilmstripWidget::updateVisibleRange);
    connect(list_->horizontalScrollBar(), &QScrollBar::valueChanged, this,
            &FilmstripWidget::updateVisibleRange);
    list_->viewport()->installEventFilter(this);
}

void FilmstripWidget::setImages(const QVector<Entry>& entries) {
    const QSignalBlocker blocker(list_);
    entries_ = entries;
    // rawThumbs_ / thumbVersions_ are intentionally NOT cleared: already-loaded
    // base images survive a refresh so re-shown items skip re-requesting. The
    // LRU keeps the store bounded across rebuilds. The shared base thumbnail is
    // keyed by imageId, so a master and its virtual copies all reuse it.

    // Incremental item sync. refreshFilmstrip fires on every rating/flag/
    // colour/filter change, every import and the 2s folderMonitor poll; tearing
    // the whole list down and re-composing every icon on the main thread each
    // time is the synchronous cost the audit flagged. Instead, take the existing
    // QListWidgetItems out (keeping the objects), re-insert the survivors in the
    // new order, and create items only for imageIds that are actually new. No
    // thumbnail is decoded here -- the visible-band request path below still
    // owns new decodes -- but the already-composed icons and badge tiles are
    // reused rather than thrown away and rebuilt.
    QList<QListWidgetItem*> pool;
    while (list_->count() > 0)
        pool.append(list_->takeItem(list_->count() - 1));
    QHash<int, QListWidgetItem*> itemById;
    for (QListWidgetItem* it : pool)
        itemById.insert(it->data(kItemRole).toInt(), it);
    QSet<QListWidgetItem*> reused;

    for (int i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        QListWidgetItem* item = nullptr;
        const auto found = itemById.find(e.imageId);
        if (found != itemById.end()) {
            item = found.value();
            itemById.erase(found);
            reused.insert(item);
        } else {
            item = new QListWidgetItem(QIcon(), QString());
            item->setSizeHint(QSize(kIconWidth, kIconHeight + 18));
        }
        QString label = QFileInfo(e.filePath).fileName();
        // A virtual copy shows its display name next to the file name so tiles sharing
        // the same file stay distinguishable under the thumbnail (the copy number
        // also appears in the page-fold badge).
        if (e.variantId != 0 && !e.variantName.empty())
            label += QStringLiteral(" · ") + QString::fromStdString(e.variantName);
        item->setText(label);
        item->setData(kItemRole, e.imageId);
        // Sequential i == the current row count, so this is an append.
        list_->insertItem(i, item);
        // No synchronous decode here -- the main thread never touches disk/embedded
        // blobs during setImages. If the caller already supplied a decoded
        // thumbnail, seed the bounded cache; otherwise the item is left
        // to the visible-window request path below.
        if (!e.thumbnail.isNull())
            storeRawThumbnail(e.imageId, e.thumbnail);
    }
    // Items that were not reused belong to imageIds that left the list.
    for (QListWidgetItem* it : pool) {
        if (!reused.contains(it))
            delete it;
    }

    // In-flight request dedupe resets: the rows have changed, so a visible item
    // may need a (re)request even if one was outstanding on the old layout.
    pendingThumbRequests_.clear();
    // Keep the badge cache across refreshes instead of wiping it on every
    // refreshFilmstrip. badgeCacheKey covers every input that changes the
    // composite (imageId, thumbnail version, rating, flag, colour label,
    // collection membership), so a refresh reuses the already-composed tiles for
    // unchanged rows and only changed/new rows pay for composition below. The
    // count-based eviction (kBadgeCacheMax) still bounds the global cache.
    // Only the visible band does any per-refresh work. updateVisibleRange
    // requests thumbnails AND composes icons for the on-screen window (+ one
    // screen of look-ahead); off-screen rows keep their previous icon / stay on
    // the placeholder until scrolled into view. Unloaded rows show placeholder
    // tiles until the async back-fill lands.
    updateVisibleRange();
    // Post-layout: MainWindow re-selects the current row (which may scroll the
    // list) right after setImages returns; re-run the window once the layout
    // settles so the selection's neighbourhood is requested too.
    QTimer::singleShot(0, this, &FilmstripWidget::updateVisibleRange);
}

void FilmstripWidget::setThumbnail(int imageId, const QImage& thumb) {
    if (thumb.isNull())
        return;
    const QVector<int> rows = rowsForImage(imageId);
    if (rows.isEmpty())
        return;
    // Back-fill entry point (MainWindow calls this after a background decode).
    // The base thumbnail is shared by the master and every virtual copy of the
    // image, so bump the per-image version and store the base exactly once, then
    // refresh every affected row (no tile keeps a stale placeholder). The
    // version bump rotates the badge-cache key so a composite built on the old
    // base image is never paired with freshly drawn badges; storeRawThumbnail
    // keeps the base in the bounded LRU + pixmap fallback.
    thumbVersions_[imageId] = thumbVersions_.value(imageId) + 1;
    storeRawThumbnail(imageId, thumb);
    pendingThumbRequests_.remove(imageId);
    for (int row : rows)
        refreshIcon(row);
}

void FilmstripWidget::setInCollection(int imageId, bool in) {
    const QVector<int> rows = rowsForImage(imageId);
    for (int row : rows) {
        Entry& e = entries_[row];
        if (in) {
            // The setter only carries a boolean; a sentinel id is enough to show
            // the collection badge (a full refresh later restores the real id
            // list).
            if (e.collectionIds.isEmpty())
                e.collectionIds.append(kCollectionSentinel);
        } else {
            e.collectionIds.clear();
        }
        refreshIcon(row);
    }
}

void FilmstripWidget::updateVisibleRange() {
    if (list_->count() == 0)
        return;
    QListWidget* lw = list_;
    // Number of columns in the wrapping grid.
    const int columns = qMax(1, lw->viewport()->width() / kGridW);
    const int viewH = qMax(1, lw->viewport()->height());
    const int scrollY = lw->verticalScrollBar()->value();
    // Visible window = current viewport plus one screen of look-ahead above and
    // below; items lie in a wrapping grid of kGridH stride.
    const int topY = qMax(0, scrollY - viewH);
    const int bottomY = scrollY + viewH * 2;
    const int firstRow = (topY / kGridH) * columns;
    const int lastRow = qMin(lw->count() - 1,
                             (bottomY / kGridH) * columns + columns - 1);
    for (int row = firstRow; row <= lastRow; ++row) {
        const int id = imageIdAt(row);
        if (id < 0)
            continue;
        // The visible band is the only place that triggers new decodes or composes
        // icons on a refresh/scroll. requestThumbnail skips rows that already
        // have a raw thumbnail (LRU or pixmap fallback); refreshIcon
        // composes the badge tile from cache when possible (cheap) or from the
        // cached raw thumbnail / placeholder otherwise.
        requestThumbnail(id);
        refreshIcon(row);
    }
}

void FilmstripWidget::requestThumbnail(int imageId) {
    if (imageId < 0)
        return;
    // Skip when the raw base is already available, either in the LRU or the pixmap
    // fallback.
    if (hasRawThumb(imageId))
        return;
    // Dedupe: no double emission per imageId.
    if (pendingThumbRequests_.contains(imageId))
        return;
    pendingThumbRequests_.insert(imageId);
    emit thumbnailRequested(imageId);
}

bool FilmstripWidget::hasRawThumb(int imageId) const {
    if (rawThumbs_.contains(imageId))
        return true;
    QPixmap tmp;
    return QPixmapCache::find(
        rawCacheKey(imageId, thumbVersions_.value(imageId)), &tmp);
}

QImage FilmstripWidget::rawThumbnail(int imageId) {
    const auto it = rawThumbs_.constFind(imageId);
    if (it != rawThumbs_.constEnd()) {
        touchLru(imageId);
        return it.value();
    }
    // Recover an evicted base from the pixmap fallback and re-seed the LRU.
    QPixmap cached;
    if (QPixmapCache::find(rawCacheKey(imageId, thumbVersions_.value(imageId)),
                           &cached) &&
        !cached.isNull()) {
        const QImage img = cached.toImage();
        storeRawThumbnail(imageId, img);
        return img;
    }
    return QImage();
}

void FilmstripWidget::storeRawThumbnail(int imageId, const QImage& thumb) {
    if (thumb.isNull())
        return;
    // Pixmap fallback keyed by thumbnail version so a changed base never reuses a
    // stale fallback.
    QPixmapCache::insert(rawCacheKey(imageId, thumbVersions_.value(imageId)),
                         QPixmap::fromImage(thumb));
    if (rawThumbs_.contains(imageId)) {
        rawThumbs_.insert(imageId, thumb);
        touchLru(imageId);
    } else {
        rawThumbs_.insert(imageId, thumb);
        lruOrder_.append(imageId);
    }
    // Evict the least-recently-used while over the cap.
    while (lruOrder_.size() > kRawThumbsMax) {
        const int evictId = lruOrder_.takeFirst();
        rawThumbs_.remove(evictId);
    }
}

void FilmstripWidget::touchLru(int imageId) {
    const int idx = lruOrder_.indexOf(imageId);
    if (idx >= 0)
        lruOrder_.move(idx, lruOrder_.size() - 1);
    else
        lruOrder_.append(imageId);
}

void FilmstripWidget::setRating(int imageId, int rating) {
    const QVector<int> rows = rowsForImage(imageId);
    for (int row : rows) {
        entries_[row].rating = rating;
        refreshIcon(row);
    }
}

void FilmstripWidget::setFlag(int imageId, int flag) {
    const QVector<int> rows = rowsForImage(imageId);
    for (int row : rows) {
        entries_[row].flag = flag;
        refreshIcon(row);
    }
}

void FilmstripWidget::setLabelColor(int imageId, int color) {
    const QVector<int> rows = rowsForImage(imageId);
    for (int row : rows) {
        entries_[row].labelColor = color;
        refreshIcon(row);
    }
}

void FilmstripWidget::setPregraded(int imageId, bool hasSubject, int score,
                                   bool suggestAuto) {
    pregradeSubjects_.insert(imageId, hasSubject);
    pregradeScores_.insert(imageId, score);
    pregradeAuto_.insert(imageId, suggestAuto);
    const QVector<int> rows = rowsForImage(imageId);
    for (int row : rows)
        refreshIcon(row);
}

void FilmstripWidget::setCollections(const QVector<lrc::Collection>& collections) {
    collections_ = collections;
}

void FilmstripWidget::selectImage(int imageId) {
    const QVector<int> rows = rowsForImage(imageId);
    if (rows.isEmpty())
        return;
    // Prefer the master row (variantId == 0) when several variants of the same
    // image are listed; the master is the natural default for a plain imageId
    // selection (restore highlight, navigate).
    int row = rows.first();
    for (int r : rows) {
        if (entries_[r].variantId == 0) {
            row = r;
            break;
        }
    }
    // Already on this row: skip the redundant setCurrentRow call (fast path).
    if (list_->currentRow() == row)
        return;
    list_->setCurrentRow(row);
}

void FilmstripWidget::selectVariant(int imageId, int variantId) {
    const QVector<int> rows = rowsForImage(imageId);
    if (rows.isEmpty())
        return;
    // Fall back to the first (master) row when the exact variant is absent.
    int row = rows.first();
    for (int r : rows) {
        if (entries_[r].variantId == variantId) {
            row = r;
            break;
        }
    }
    // Already on this row: skip the redundant setCurrentRow call (fast path).
    if (list_->currentRow() == row)
        return;
    list_->setCurrentRow(row);
}

int FilmstripWidget::imageIdAt(int row) const {
    QListWidgetItem* item = list_->item(row);
    return item ? item->data(kItemRole).toInt() : -1;
}

FilmstripWidget::Entry* FilmstripWidget::entryAt(int row) {
    if (row < 0 || row >= entries_.size())
        return nullptr;
    return &entries_[row];
}

const FilmstripWidget::Entry* FilmstripWidget::entryAt(int row) const {
    if (row < 0 || row >= entries_.size())
        return nullptr;
    return &entries_[row];
}

QVector<int> FilmstripWidget::rowsForImage(int imageId) const {
    QVector<int> rows;
    for (int i = 0; i < entries_.size(); ++i) {
        if (entries_[i].imageId == imageId)
            rows.append(i);
    }
    return rows;
}

void FilmstripWidget::refreshIcon(int row) {
    Entry* e = entryAt(row);
    if (!e)
        return;
    QListWidgetItem* item = list_->item(row);
    if (!item)
        return;
    // The key covers every input that changes the composite appearance:
    // imageId, thumbnail version (rotated by setThumbnail), rating, flag,
    // colour label, collection membership and variant id. A rating/flag/label/
    // collection change simply produces a different key, so old entries are
    // never wrongly reused and nothing needs explicit removal for those setters.
    // The variant id keeps the master and its virtual copies (same imageId)
    // in separate cache entries.
    const QString key = badgeCacheKey(
        e->imageId, thumbVersions_.value(e->imageId), e->rating, e->flag,
        e->labelColor, !e->collectionIds.isEmpty(), pregradeState(e->imageId),
        e->variantId, pregradeAuto_.value(e->imageId, false));
    QPixmap cached;
    if (QPixmapCache::find(key, &cached)) {
        item->setIcon(QIcon(cached));
        return;
    }
    // Compose badges from the raw (un-badged) thumbnail so removing a badge
    // never leaves a stale copy behind. rawThumbnail serves the LRU first and
    // falls back to the pixmap raw cache after an LRU eviction, then seed the
    // badge cache so later refreshes (e.g. repeated rating changes on the same
    // image) skip the composition entirely.
    const QImage thumb = rawThumbnail(e->imageId);
    const QPixmap pm = makeBadgedIcon(thumb, e->rating, e->flag, e->labelColor,
                                      !e->collectionIds.isEmpty(),
                                      pregradeState(e->imageId),
                                      pregradeAuto_.value(e->imageId, false),
                                      e->variantId);
    insertBadgePixmap(key, pm);
    item->setIcon(QIcon(pm));
}

// Maps the stored B-2 pregrade marker to a badge state: 0 = none, 1 = pregraded
// without a subject, 2 = pregraded with a detected subject.
int FilmstripWidget::pregradeState(int imageId) const {
    const int score = pregradeScores_.value(imageId, -1);
    if (score < 0)
        return 0;
    return pregradeSubjects_.value(imageId, false) ? 2 : 1;
}

int FilmstripWidget::currentImageId() const {
    return imageIdAt(list_->currentRow());
}

int FilmstripWidget::currentVariantId() const {
    const Entry* e = entryAt(list_->currentRow());
    return e ? e->variantId : 0;
}

int FilmstripWidget::count() const { return list_->count(); }

void FilmstripWidget::onCurrentChanged(int row) {
    const int id = imageIdAt(row);
    if (id < 0)
        return;
    emit selectionChanged(id);
    // Carry the clicked entry's variant id so MainWindow can load the matching
    // variant edit stack (0 = master). Emitted on every selection so the new
    // signal alone is a complete replacement for selectionChanged.
    const Entry* e = entryAt(row);
    emit imageVariantSelected(id, e ? e->variantId : 0);
}

bool FilmstripWidget::eventFilter(QObject* obj, QEvent* event) {
    // Recompute the visible window after the list viewport resizes; deferred a turn
    // so the new layout is applied first.
    if (obj == list_->viewport() && event->type() == QEvent::Resize)
        QTimer::singleShot(0, this, &FilmstripWidget::updateVisibleRange);
    return QWidget::eventFilter(obj, event);
}

void FilmstripWidget::keyPressEvent(QKeyEvent* event) {
    const int id = currentImageId();
    if (id >= 0) {
        const int key = event->key();
        const Qt::KeyboardModifiers mods = event->modifiers();
        const bool ctrl = mods.testFlag(Qt::ControlModifier);
        const bool shift = mods.testFlag(Qt::ShiftModifier);
        const bool alt = mods.testFlag(Qt::AltModifier);

        if (key >= Qt::Key_1 && key <= Qt::Key_5) {
            emit ratingRequested(id, key - Qt::Key_0);
            return;
        }
        // Plain 0 clears the rating; Ctrl+0 belongs to the window layer and
        // Ctrl+Shift+0 clears the colour label (handled below).
        if (key == Qt::Key_0 && !ctrl && !shift && !alt) {
            emit ratingRequested(id, 0);
            return;
        }
        if (key == Qt::Key_P) {
            emit flagRequested(id, lrc::ImageFlag::FlagPicked);
            return;
        }
        if (key == Qt::Key_X) {
            emit flagRequested(id, lrc::ImageFlag::FlagRejected);
            return;
        }
        if (key == Qt::Key_U) {
            emit flagRequested(id, lrc::ImageFlag::FlagNone);
            return;
        }
        if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
            emit removeRequested(id);
            return;
        }

        // Colour-label shortcuts.
        // 6/7/8/9 -> label 1/2/3/4 (bare keys).
        if (!ctrl && !shift && !alt && key >= Qt::Key_6 && key <= Qt::Key_9) {
            emit labelColorRequested(id, key - Qt::Key_5);
            return;
        }
        // Ctrl+6/7/8 -> label 5/6/7.
        if (ctrl && !shift && !alt && key >= Qt::Key_6 && key <= Qt::Key_8) {
            emit labelColorRequested(id, key - Qt::Key_1);
            return;
        }
        // Ctrl+Shift+0 -> clear colour label.
        if (ctrl && shift && !alt && key == Qt::Key_0) {
            emit labelColorRequested(id, 0);
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

void FilmstripWidget::contextMenuEvent(QContextMenuEvent* event) {
    const int row = list_->indexAt(event->pos()).row();
    const int id = imageIdAt(row);
    if (id < 0)
        return;
    list_->setCurrentRow(row);
    const Entry* e = entryAt(row);

    QMenu menu(this);

    // Rating.
    auto* ratingMenu = menu.addMenu(tr("评分"));
    for (int r = 0; r <= 5; ++r) {
        QAction* act = ratingMenu->addAction(
            r == 0 ? tr("清除评分") : tr("%1 星").arg(r));
        act->setCheckable(true);
        act->setChecked(e && e->rating == r);
        connect(act, &QAction::triggered, this,
                [this, id, r]() { emit ratingRequested(id, r); });
    }

    // Flag.
    auto* flagMenu = menu.addMenu(tr("旗标"));
    QAction* pick = flagMenu->addAction(tr("选取 (P)"));
    pick->setCheckable(true);
    pick->setChecked(e && e->flag == lrc::ImageFlag::FlagPicked);
    connect(pick, &QAction::triggered, this,
            [this, id]() { emit flagRequested(id, lrc::ImageFlag::FlagPicked); });
    QAction* none = flagMenu->addAction(tr("无旗标 (U)"));
    none->setCheckable(true);
    none->setChecked(e && e->flag == lrc::ImageFlag::FlagNone);
    connect(none, &QAction::triggered, this,
            [this, id]() { emit flagRequested(id, lrc::ImageFlag::FlagNone); });
    QAction* reject = flagMenu->addAction(tr("拒绝 (X)"));
    reject->setCheckable(true);
    reject->setChecked(e && e->flag == lrc::ImageFlag::FlagRejected);
    connect(reject, &QAction::triggered, this,
            [this, id]() { emit flagRequested(id, lrc::ImageFlag::FlagRejected); });

    // Colour label.
    auto* labelMenu = menu.addMenu(tr("色标"));
    QAction* clearLabel = labelMenu->addAction(tr("清除色标"));
    connect(clearLabel, &QAction::triggered, this,
            [this, id]() { emit labelColorRequested(id, 0); });
    labelMenu->addSeparator();
    const int curLabel = e ? e->labelColor : 0;
    for (int v = 1; v <= 7; ++v) {
        QAction* act = labelMenu->addAction(tr(kLabelColorNames[v - 1]));
        act->setCheckable(true);
        act->setChecked(curLabel == v);
        connect(act, &QAction::triggered, this, [this, id, v, curLabel]() {
            // Clicking the active label clears it.
            emit labelColorRequested(id, v == curLabel ? 0 : v);
        });
    }

    // Collections.
    auto* collectionMenu = menu.addMenu(tr("收藏夹"));
    if (collections_.isEmpty()) {
        QAction* noneCol = collectionMenu->addAction(tr("（无收藏夹）"));
        noneCol->setEnabled(false);
    } else {
        for (const lrc::Collection& c : collections_) {
            const int cid = static_cast<int>(c.id);
            const bool inCol = e && e->collectionIds.contains(cid);
            QAction* act = collectionMenu->addAction(
                c.name.empty() ? tr("未命名收藏夹")
                               : QString::fromStdString(c.name));
            act->setCheckable(true);
            act->setChecked(inCol);
            connect(act, &QAction::triggered, this, [this, id, cid, inCol]() {
                emit collectionRequested(id, cid, !inCol);
            });
        }
    }

    menu.addSeparator();
    menu.addAction(tr("从目录移除"), this,
                   [this, id]() { emit removeRequested(id); });
    menu.exec(event->globalPos());
}
