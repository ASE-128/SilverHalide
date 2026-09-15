#pragma once

// Visual hue/saturation picker used by the split-toning panel. Angle selects
// hue, radius selects saturation (per LR's "可视化颜色调整"). Clicking or
// dragging updates the colour and emits colorChanged.

#include <QWidget>

class QMouseEvent;
class QResizeEvent;

class ColorWheelWidget : public QWidget {
    Q_OBJECT
public:
    explicit ColorWheelWidget(QWidget* parent = nullptr);

    // hueDeg in [0,360), saturationPct in [0,100].
    void setColor(double hueDeg, double saturationPct);
    double hue() const { return hue_; }
    double saturation() const { return saturation_; }

signals:
    void colorChanged(double hueDeg, double saturationPct);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    QPointF center() const;
    double maxRadius() const;
    void pick(const QPointF& pos);
    void renderWheel();

    double hue_ = 0.0;
    double saturation_ = 100.0;
    QImage wheel_;
    int wheelSize_ = -1;
};
