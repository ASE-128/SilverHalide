#include "ui/CurveEditor.h"

#include <QFont>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

#include "engine/ZoneCurve.h"

namespace {

constexpr double kMargin = 6.0;      // plot inset, px
constexpr double kLabelStrip = 13.0;  // bottom strip holding the zone names, px

// Zone display names, in shadows..highlights order (match Lightroom).
// QT_TR_NOOP keeps the literals extractable for translation; the lookup itself
// happens at the draw site, where a QObject translation context exists.
const char* kZoneNames[lrc::zonecurve::kZoneCount] = {
    QT_TR_NOOP("阴影"), QT_TR_NOOP("暗色调"), QT_TR_NOOP("亮色调"),
    QT_TR_NOOP("高光")};

// The x-range a zone handle may be dragged over. Each zone is confined to its
// own band (bounded by the midpoints of its neighbours), so the four handles
// keep their order and the curve always remains a function.
void zoneXRange(int i, float& lo, float& hi) {
    const auto ax = lrc::zonecurve::anchors();
    const int count = static_cast<int>(lrc::zonecurve::kZoneCount);
    lo = i == 0 ? 0.0f : (ax[i - 1] + ax[i]) * 0.5f;
    hi = i == count - 1 ? 1.0f : (ax[i] + ax[i + 1]) * 0.5f;
}

}  // namespace

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent) {
    setMinimumSize(140, 100);
    resetToLinear();
}

void CurveEditor::resetToLinear() {
    points_ = lrc::zonecurve::makeLinear();
    update();
}

void CurveEditor::setPoints(const std::vector<lrc::ToneCurvePoint>& points) {
    // A missing curve means identity; make sure switching images never leaves a
    // stale curve on screen.
    if (points.size() < 2) {
        resetToLinear();
        return;
    }
    // Accept point sets that already follow the zone layout (black and white
    // endpoints at the corners, one handle inside each band) unchanged.
    // Anything else, e.g. curves saved by the older free-form editor, is
    // quantized onto the four zone handles.
    bool zoneLayout = points.size() == lrc::zonecurve::kZoneCount + 2;
    constexpr float kEps = 1e-4f;
    if (zoneLayout &&
        (std::abs(points[0].x) > kEps || std::abs(points[0].y) > kEps ||
         std::abs(points.back().x - 1.0f) > kEps ||
         std::abs(points.back().y - 1.0f) > kEps)) {
        zoneLayout = false;
    }
    for (size_t i = 0; zoneLayout && i < lrc::zonecurve::kZoneCount; ++i) {
        float lo, hi;
        zoneXRange(static_cast<int>(i), lo, hi);
        const float x = points[i + 1].x;
        if (x < lo - kEps || x > hi + kEps)
            zoneLayout = false;
    }
    points_ = zoneLayout ? points : lrc::zonecurve::fromPoints(points);
    update();
}

QRectF CurveEditor::plotArea() const {
    return QRectF(kMargin, kMargin, std::max(1.0, width() - 2 * kMargin),
                  std::max(1.0, height() - 2 * kMargin - kLabelStrip));
}

QPointF CurveEditor::toWidget(const lrc::ToneCurvePoint& p) const {
    const QRectF area = plotArea();
    return QPointF(area.left() + p.x * area.width(),
                   area.bottom() - p.y * area.height());
}

lrc::ToneCurvePoint CurveEditor::toNorm(const QPointF& p) const {
    const QRectF area = plotArea();
    const double x = std::clamp((p.x() - area.left()) / area.width(), 0.0, 1.0);
    const double y =
        std::clamp((area.bottom() - p.y()) / area.height(), 0.0, 1.0);
    return {static_cast<float>(x), static_cast<float>(y)};
}

int CurveEditor::hitTest(const QPointF& pos) const {
    for (size_t i = 0; i < lrc::zonecurve::kZoneCount; ++i) {
        const QPointF w = toWidget(points_[i + 1]);
        if (QLineF(w, pos).length() <= 9.0)
            return static_cast<int>(i);
    }
    return -1;
}

void CurveEditor::emitChanged() { emit curveChanged(points_); }

void CurveEditor::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(28, 28, 30));
    const QRectF area = plotArea();

    // Grid + diagonal reference.
    painter.setPen(QColor(255, 255, 255, 26));
    for (int g = 1; g < 8; ++g) {
        const double x = area.left() + area.width() * g / 8.0;
        const double y = area.top() + area.height() * g / 8.0;
        painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    }
    painter.setPen(QColor(255, 255, 255, 46));
    painter.drawLine(area.bottomLeft(), area.topRight());

    // Faint separators between the four tonal bands, Lightroom-style.
    painter.setPen(QColor(255, 255, 255, 30));
    const auto ax = lrc::zonecurve::anchors();
    for (size_t i = 0; i + 1 < lrc::zonecurve::kZoneCount; ++i) {
        const double x = area.left() + (ax[i] + ax[i + 1]) * 0.5f * area.width();
        painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
    }

    // Curve, sampled with the same monotone interpolant the renderer uses so
    // the widget is a faithful preview of the pipeline output.
    QPainterPath path;
    constexpr int kSamples = 128;
    for (int s = 0; s <= kSamples; ++s) {
        const float x = static_cast<float>(s) / kSamples;
        const float y = lrc::zonecurve::interpolate(points_, x);
        const QPointF w = toWidget(lrc::ToneCurvePoint{x, y});
        if (s == 0)
            path.moveTo(w);
        else
            path.lineTo(w);
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(200, 200, 220), 1.6));
    painter.drawPath(path);

    // Fixed endpoints as small squares. Named topRight rather than the obvious
    // `tr`, which would shadow QObject::tr() for the rest of paintEvent.
    const QPointF bl = toWidget(points_.front());
    const QPointF topRight = toWidget(points_.back());
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 180));
    painter.drawRect(QRectF(bl.x() - 2.5, bl.y() - 2.5, 5, 5));
    painter.drawRect(
        QRectF(topRight.x() - 2.5, topRight.y() - 2.5, 5, 5));

    // Zone handles; the one being dragged is highlighted.
    for (size_t i = 0; i < lrc::zonecurve::kZoneCount; ++i) {
        const QPointF w = toWidget(points_[i + 1]);
        const bool active = static_cast<int>(i) == dragIndex_;
        painter.setPen(QColor(255, 255, 255, 200));
        painter.setBrush(active ? QColor(120, 170, 240) : QColor(70, 110, 190));
        painter.drawEllipse(w, active ? 5.0 : 3.5, active ? 5.0 : 3.5);
    }

    // Zone names in the strip below the plot, tracking each handle.
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 8));
    painter.setPen(QColor(255, 255, 255, 150));
    const double labelY = height() - kLabelStrip * 0.5;
    for (size_t i = 0; i < lrc::zonecurve::kZoneCount; ++i) {
        const double x = area.left() + points_[i + 1].x * area.width();
        const QRectF lr(x - area.width() * 0.25, labelY - 7,
                        area.width() * 0.5, 14);
        painter.drawText(lr, Qt::AlignCenter, tr(kZoneNames[i]));
    }
}

void CurveEditor::mousePressEvent(QMouseEvent* event) {
    dragIndex_ = hitTest(event->pos());
    dragged_ = false;
    if (dragIndex_ >= 0)
        emit dragStarted();
}

void CurveEditor::mouseMoveEvent(QMouseEvent* event) {
    if (dragIndex_ < 0)
        return;
    const int i = dragIndex_;
    lrc::ToneCurvePoint p = toNorm(event->pos());

    // Horizontal drag stays inside this zone's band so the handles keep their
    // order; vertical drag is clamped between the neighbouring points to keep
    // the curve monotonic, exactly like Lightroom's point curve.
    float lo, hi;
    zoneXRange(i, lo, hi);
    p.x = std::clamp(p.x, lo, hi);
    const float loY = points_[static_cast<size_t>(i)].y;      // lower neighbour
    const float hiY = points_[static_cast<size_t>(i) + 2].y;  // upper neighbour
    p.y = std::clamp(p.y, std::min(loY, hiY), std::max(loY, hiY));

    points_[static_cast<size_t>(i) + 1] = p;
    dragged_ = true;
    update();
    emitChanged();  // live preview through the develop panel's 50 ms debounce
}

void CurveEditor::mouseReleaseEvent(QMouseEvent*) {
    const bool hadGrab = (dragIndex_ >= 0);
    if (hadGrab && dragged_) {
        emitChanged();
        emit editingFinished();
    }
    dragIndex_ = -1;
    dragged_ = false;
    if (hadGrab)
        emit dragEnded();
}

void CurveEditor::mouseDoubleClickEvent(QMouseEvent* event) {
    const int hit = hitTest(event->pos());
    if (hit < 0)
        return;
    // Double-clicking a zone handle resets that zone to linear.
    const auto ax = lrc::zonecurve::anchors();
    points_[static_cast<size_t>(hit) + 1] = {ax[hit], ax[hit]};
    dragged_ = false;
    emitChanged();
    emit editingFinished();
    update();
}
