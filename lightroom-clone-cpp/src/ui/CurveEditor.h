#pragma once

// Lightroom-style zone tone-curve editor. Four draggable zone handles -
// shadows / darks / lights / highlights - are pinned between fixed black and
// white endpoints. Dragging a handle raises or lowers its tonal band while the
// monotonicity constraint keeps the curve from folding over. Double-clicking a
// handle resets that zone to linear. Curve output feeds the pipeline's
// tone-curve stage.

#include <QWidget>
#include <vector>

#include "engine/Adjustments.h"

class CurveEditor : public QWidget {
    Q_OBJECT
public:
    explicit CurveEditor(QWidget* parent = nullptr);

    // Returns the curve as black endpoint + four zones + white endpoint.
    std::vector<lrc::ToneCurvePoint> points() const { return points_; }
    // Loads a curve. Zone curves are kept as-is; arbitrary point sets from the
    // older free-form editor are quantized onto the four zone handles.
    void setPoints(const std::vector<lrc::ToneCurvePoint>& points);
    void resetToLinear();

signals:
    void curveChanged(const std::vector<lrc::ToneCurvePoint>& points);
    // A zone handle was pressed: a drag gesture started. The develop panel
    // switches to its coarse (30 ms) preview throttle for the duration.
    void dragStarted();
    // The grab was released (with or without an actual move). The develop panel
    // restores the refine throttle and re-schedules the final preview pass.
    void dragEnded();
    void editingFinished();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QRectF plotArea() const;
    QPointF toWidget(const lrc::ToneCurvePoint& p) const;
    lrc::ToneCurvePoint toNorm(const QPointF& p) const;
    // Returns the zone index (0..3) whose handle is near `pos`, or -1.
    int hitTest(const QPointF& pos) const;
    void emitChanged();

    // Six points: endpoints plus one handle per zone at indices 1..4.
    std::vector<lrc::ToneCurvePoint> points_;
    int dragIndex_ = -1;  // zone handle (0..3) being dragged, or -1
    bool dragged_ = false;
};
