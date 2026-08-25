#include "ui/ColorWheelWidget.h"

#include <QMouseEvent>
#include <QPainter>

#include "utils/MathUtils.h"

namespace {
constexpr int kMinSize = 96;
constexpr int kMarkerRadius = 5;
} // namespace

ColorWheelWidget::ColorWheelWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumSize(kMinSize, kMinSize);
    setCursor(Qt::CrossCursor);
}

QPointF ColorWheelWidget::center() const {
    return QPointF(width() / 2.0, height() / 2.0);
}

double ColorWheelWidget::maxRadius() const {
    return std::min(width(), height()) / 2.0 - 4.0;
}

void ColorWheelWidget::setColor(double hueDeg, double saturationPct) {
    hue_ = hueDeg;
    saturation_ = std::clamp(saturationPct, 0.0, 100.0);
    update();
}

void ColorWheelWidget::pick(const QPointF& pos) {
    const QPointF c = center();
    const double dx = pos.x() - c.x();
    const double dy = pos.y() - c.y();
    const double r = std::sqrt(dx * dx + dy * dy);
    if (r < 1e-3) {
        setColor(hue_, 100.0);
        emit colorChanged(hue_, saturation_);
        return;
    }
    double a = std::atan2(dy, dx) * 180.0 / lrc::kPi;
    if (a < 0.0)
        a += 360.0;
    const double s = std::clamp(r / maxRadius(), 0.0, 1.0) * 100.0;
    hue_ = a;
    saturation_ = s;
    emit colorChanged(hue_, saturation_);
}

void ColorWheelWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        pick(event->pos());
        event->accept();
    }
}

void ColorWheelWidget::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) {
        pick(event->pos());
        event->accept();
    }
}

void ColorWheelWidget::renderWheel() {
    const int size = std::max(kMinSize, std::min(width(), height()));
    wheel_ = QImage(size, size, QImage::Format_RGB32);
    const double cx = (size - 1) / 2.0;
    const double maxR = cx - 3.0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double dx = x - cx;
            const double dy = y - cx;
            const double r = std::sqrt(dx * dx + dy * dy);
            if (r > maxR) {
                wheel_.setPixel(x, y, 0xFF262626);
                continue;
            }
            double a = std::atan2(dy, dx) * 180.0 / lrc::kPi;
            if (a < 0.0)
                a += 360.0;
            const int sat = static_cast<int>(r / maxR * 255.0);
            // A slightly lifted lightness reads better on the dark theme.
            wheel_.setPixel(x, y, QColor::fromHsl(static_cast<int>(a), sat, 135)
                                      .rgb());
        }
    }
    wheelSize_ = size;
}

void ColorWheelWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0x26, 0x26, 0x26));
    if (wheelSize_ != std::max(kMinSize, std::min(width(), height())))
        renderWheel();
    if (wheel_.isNull())
        return;

    const int size = wheel_.width();
    painter.drawImage(QRect((width() - size) / 2, (height() - size) / 2,
                            size, size),
                      wheel_);

    // Marker at the current hue/saturation.
    const double ang = hue_ * lrc::kPi / 180.0;
    const double r = maxRadius() * saturation_ / 100.0;
    const QPointF c = center();
    const QPointF m = c + QPointF(std::cos(ang), std::sin(ang)) * r;
    painter.setPen(QPen(Qt::white, 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(m, kMarkerRadius, kMarkerRadius);
    painter.setPen(Qt::black);
    painter.drawEllipse(m, kMarkerRadius - 1, kMarkerRadius - 1);
}

void ColorWheelWidget::resizeEvent(QResizeEvent*) { update(); }
