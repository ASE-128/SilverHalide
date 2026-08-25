#include "ui/HistogramWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace {

// Builds 3 x 256 bins from an interleaved RGB8 buffer, sampled at most every
// ~4 pixels so very large previews stay cheap.
std::array<HistogramWidget::ChannelBins, 3> histogramFromRgb8(
    const uint8_t* data, int width, int height) {
    std::array<HistogramWidget::ChannelBins, 3> bins{};
    const int64_t pixels = static_cast<int64_t>(width) * height;
    const int step = static_cast<int>(std::max<int64_t>(1, pixels / 4000000));
    for (int64_t i = 0; i < pixels; i += step) {
        const uint8_t* p = data + i * 3;
        ++bins[0][p[0]];
        ++bins[1][p[1]];
        ++bins[2][p[2]];
    }
    return bins;
}

} // namespace

HistogramWidget::HistogramWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(120, 80);
    setToolTip(tr("RGB histogram. Double-click to toggle log scale."));
}

void HistogramWidget::setHistogram(
    const std::array<ChannelBins, 3>& histogram) {
    histogram_ = histogram;
    update();
}

void HistogramWidget::setImage(const QImage& image) {
    if (image.isNull()) {
        clear();
        return;
    }
    // 非 RGB888 输入先转一次，避免直方图莫名空白 (normalize common formats).
    const QImage rgb =
        image.format() == QImage::Format_RGB888
            ? image
            : image.convertToFormat(QImage::Format_RGB888);
    const auto* p = rgb.constBits();
    const auto bins = histogramFromRgb8(p, rgb.width(), rgb.height());
    setHistogram(bins);
}

void HistogramWidget::computeFromBuffer(const lrc::ImageBufferPtr& rgb) {
    if (!rgb || rgb->format() != lrc::PixelFormat::U8 || rgb->empty()) {
        clear();
        return;
    }
    const auto bins = histogramFromRgb8(rgb->data().data(), rgb->width(),
                                        rgb->height());
    setHistogram(bins);
}

void HistogramWidget::clear() {
    for (auto& ch : histogram_)
        ch.fill(0);
    update();
}

void HistogramWidget::toggleLogScale(bool enabled) {
    if (logScale_ == enabled)
        return;
    logScale_ = enabled;
    emit logScaleChanged(enabled);
    update();
}

void HistogramWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        toggleLogScale(!logScale_);
        event->accept();
    }
}

void HistogramWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const QColor bg(30, 30, 30);
    painter.fillRect(rect(), bg);

    if (rect().width() < 8 || rect().height() < 8)
        return;

    // Global maximum bin count across all channels (log-scaled if enabled).
    uint32_t maxCount = 1;
    for (const auto& ch : histogram_)
        for (uint32_t v : ch)
            maxCount = std::max(maxCount, v);

    const QColor colors[3] = {QColor(220, 60, 60, 150), QColor(60, 190, 60, 150),
                              QColor(70, 70, 220, 150)};
    const double w = rect().width();
    const double h = rect().height();
    const double pad = 2.0;

    // Grid at 25% / 50% / 75%.
    painter.setPen(QColor(255, 255, 255, 24));
    for (int g = 1; g <= 3; ++g) {
        const double y = h * (g / 4.0);
        painter.drawLine(QPointF(0, y), QPointF(w, y));
    }

    const auto scaleValue = [&](uint32_t v) {
        const double norm = static_cast<double>(v) / maxCount;
        if (logScale_)
            return std::log1p(9.0 * norm) / std::log(10.0);
        return norm;
    };

    for (int ch = 0; ch < 3; ++ch) {
        QPainterPath path;
        const auto& bins = histogram_[ch];
        for (int i = 0; i < 256; ++i) {
            const double x = pad + i * (w - 2 * pad) / 255.0;
            const double y = h - 1.0 - scaleValue(bins[i]) * (h - pad);
            if (i == 0)
                path.moveTo(x, y);
            else
                path.lineTo(x, y);
        }
        path.lineTo(w - pad, h - 1.0);
        path.lineTo(pad, h - 1.0);
        path.closeSubpath();
        painter.fillPath(path, colors[ch]);
    }

    // Luminance (white) channel drawn on top, LR-style: per-bin max of RGB.
    QPainterPath lumPath;
    for (int i = 0; i < 256; ++i) {
        const uint32_t v = std::max({histogram_[0][i], histogram_[1][i],
                                     histogram_[2][i]});
        const double x = pad + i * (w - 2 * pad) / 255.0;
        const double y = h - 1.0 - scaleValue(v) * (h - pad);
        if (i == 0)
            lumPath.moveTo(x, y);
        else
            lumPath.lineTo(x, y);
    }
    lumPath.lineTo(w - pad, h - 1.0);
    lumPath.lineTo(pad, h - 1.0);
    lumPath.closeSubpath();
    painter.fillPath(lumPath, QColor(235, 235, 235, 90));
}
