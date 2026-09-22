#pragma once

/**
 * @brief Self-drawn RGB histogram.
 *
 * Three channels are overlaid as translucent filled curves; a luminance
 * (white) channel is drawn on top. Supports a linear / logarithmic vertical
 * scale toggle.
 *
 * Primary data source (P3-1): bins computed by Pipeline::computeHistogramFromLinear
 * from the adjusted linear F32 display buffer on the preview-worker thread and
 * delivered via setHistogram -- exact counts, no sampling. setImage remains as
 * the 8-bit fallback path (full-res 1:1 / coarse frames) and also traverses
 * every pixel.
 */

#include <QImage>
#include <QWidget>

#include <array>

#include "engine/ImageBuffer.h"

class HistogramWidget : public QWidget {
    Q_OBJECT
public:
    using ChannelBins = std::array<uint32_t, 256>;

    explicit HistogramWidget(QWidget* parent = nullptr);

    void setHistogram(const std::array<ChannelBins, 3>& histogram);
    void setImage(const QImage& image);
    void clear();

    bool logScale() const { return logScale_; }

public slots:
    void toggleLogScale(bool enabled);

signals:
    void logScaleChanged(bool enabled);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void computeFromBuffer(const lrc::ImageBufferPtr& rgb);

    std::array<ChannelBins, 3> histogram_{};
    bool logScale_ = false;
};
