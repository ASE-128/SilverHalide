#include "engine/ImageBuffer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "engine/ColorManager.h"
#include "utils/Parallel.h"

namespace lrc {

namespace {

// Returns the interleaved component stride of the given buffer.
inline size_t rowBytes(const ImageBuffer& b) {
    return static_cast<size_t>(b.width()) * static_cast<size_t>(b.channels());
}

// Round-to-nearest quantization (truncation would bias dark).
inline uint16_t toU16(float v) {
    return static_cast<uint16_t>(std::clamp(v, 0.0f, 1.0f) * 65535.0f + 0.5f);
}

inline uint8_t toU8(float v) {
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

void loadRowFloats(const ImageBuffer& src, int y, std::vector<float>& row) {
    const int w = src.width();
    const int ch = src.channels();
    row.resize(static_cast<size_t>(w) * static_cast<size_t>(ch));
    switch (src.format()) {
    case PixelFormat::F32: {
        const float* p = reinterpret_cast<const float*>(src.data().data()) +
                         static_cast<size_t>(y) * w * ch;
        std::copy(p, p + row.size(), row.begin());
        break;
    }
    case PixelFormat::U16: {
        const uint16_t* p = reinterpret_cast<const uint16_t*>(src.data().data()) +
                            static_cast<size_t>(y) * w * ch;
        for (size_t i = 0; i < row.size(); ++i)
            row[i] = static_cast<float>(p[i]) / 65535.0f;
        break;
    }
    case PixelFormat::U8: {
        const uint8_t* p = src.data().data() + static_cast<size_t>(y) * w * ch;
        for (size_t i = 0; i < row.size(); ++i)
            row[i] = static_cast<float>(p[i]) / 255.0f;
        break;
    }
    }
}

void writeRowFloats(ImageBuffer& dst, int y, const std::vector<float>& row) {
    const int w = dst.width();
    const int ch = dst.channels();
    switch (dst.format()) {
    case PixelFormat::F32: {
        float* p = reinterpret_cast<float*>(dst.data().data()) +
                   static_cast<size_t>(y) * w * ch;
        std::copy(row.begin(), row.end(), p);
        break;
    }
    case PixelFormat::U16: {
        uint16_t* p = reinterpret_cast<uint16_t*>(dst.data().data()) +
                      static_cast<size_t>(y) * w * ch;
        for (size_t i = 0; i < row.size(); ++i)
            p[i] = toU16(row[i]);
        break;
    }
    case PixelFormat::U8: {
        uint8_t* p = dst.data().data() + static_cast<size_t>(y) * w * ch;
        for (size_t i = 0; i < row.size(); ++i)
            p[i] = toU8(row[i]);
        break;
    }
    }
}

} // namespace

ImageBuffer::ImageBuffer(int width, int height, PixelFormat format)
    : width_(width), height_(height), format_(format) {
    if (width < 0 || height < 0)
        throw std::invalid_argument("ImageBuffer: negative dimensions");
    data_.resize(static_cast<size_t>(width) * height * channels_ *
                 (format == PixelFormat::U16 ? 2u : (format == PixelFormat::U8 ? 1u : 4u)));
}

std::shared_ptr<ImageBuffer> ImageBuffer::create(int width, int height,
                                                 PixelFormat format) {
    return std::make_shared<ImageBuffer>(width, height, format);
}

float ImageBuffer::pixel(int x, int y, int channel) const {
    if (x < 0 || x >= width_ || y < 0 || y >= height_ ||
        channel < 0 || channel >= channels_)
        return 0.0f;
    const size_t idx = (static_cast<size_t>(y) * width_ + x) * channels_ + channel;
    switch (format_) {
    case PixelFormat::U16:
        return reinterpret_cast<const uint16_t*>(data_.data())[idx] / 65535.0f;
    case PixelFormat::F32:
        return reinterpret_cast<const float*>(data_.data())[idx];
    case PixelFormat::U8:
        return data_[idx] / 255.0f;
    }
    return 0.0f;
}

void ImageBuffer::setPixel(int x, int y, int channel, float value) {
    if (x < 0 || x >= width_ || y < 0 || y >= height_ ||
        channel < 0 || channel >= channels_)
        return;
    const size_t idx = (static_cast<size_t>(y) * width_ + x) * channels_ + channel;
    switch (format_) {
    case PixelFormat::U16:
        reinterpret_cast<uint16_t*>(data_.data())[idx] = toU16(value);
        break;
    case PixelFormat::F32:
        reinterpret_cast<float*>(data_.data())[idx] = value;
        break;
    case PixelFormat::U8:
        data_[idx] = toU8(value);
        break;
    }
}

std::shared_ptr<ImageBuffer> ImageBuffer::crop(int x, int y, int width,
                                               int height) const {
    if (width <= 0 || height <= 0 || x < 0 || y < 0 ||
        x + width > width_ || y + height > height_)
        return ImageBuffer::create(0, 0, format_);
    auto out = ImageBuffer::create(width, height, format_);
    const size_t ch = static_cast<size_t>(channels_);
    const size_t srcRow = static_cast<size_t>(width_) * ch;
    const size_t dstRow = static_cast<size_t>(width) * ch;
    const size_t elemBytes = format_ == PixelFormat::U16 ? 2u
                             : (format_ == PixelFormat::U8 ? 1u : 4u);
    const uint8_t* src = data_.data() +
                         (static_cast<size_t>(y) * width_ + x) * ch * elemBytes;
    uint8_t* dst = out->data_.data();
    for (int yy = 0; yy < height; ++yy) {
        std::memcpy(dst, src, dstRow * elemBytes);
        src += srcRow * elemBytes;
        dst += dstRow * elemBytes;
    }
    return out;
}

std::shared_ptr<ImageBuffer> ImageBuffer::resize(int newWidth,
                                                 int newHeight) const {
    return resizeToFormat(newWidth, newHeight, format_);
}

std::shared_ptr<ImageBuffer> ImageBuffer::resizeToF32(int newWidth,
                                                      int newHeight) const {
    if (newWidth <= 0 || newHeight <= 0 || empty())
        return ImageBuffer::create(0, 0, PixelFormat::F32);
    // Same dimensions: a plain float conversion, no averaging to do.
    if (newWidth == width_ && newHeight == height_)
        return toF32();
    return resizeToFormat(newWidth, newHeight, PixelFormat::F32);
}

std::shared_ptr<ImageBuffer> ImageBuffer::resizeToFormat(
    int newWidth, int newHeight, PixelFormat format) const {
    if (newWidth <= 0 || newHeight <= 0 || empty())
        return ImageBuffer::create(0, 0, format);
    if (newWidth == width_ && newHeight == height_ && format == format_)
        return std::make_shared<ImageBuffer>(*this);

    const int ch = channels_;
    const double scaleX = static_cast<double>(width_) / newWidth;
    const double scaleY = static_cast<double>(height_) / newHeight;

    // Intermediate float image after the horizontal pass. Each source row is
    // independent, so rows split across the thread pool with per-chunk scratch.
    auto hpass = ImageBuffer::create(newWidth, height_, PixelFormat::F32);
    const size_t rowGrain =
        lrc::parallel::defaultGrain(static_cast<size_t>(height_));
    lrc::parallel::parallelFor(static_cast<size_t>(height_), rowGrain,
                               [&](size_t by, size_t ey) {
        std::vector<float> inRow;
        std::vector<float> outRow(static_cast<size_t>(newWidth) * ch);
        for (int y = static_cast<int>(by); y < static_cast<int>(ey); ++y) {
            loadRowFloats(*this, y, inRow);
            for (int ox = 0; ox < newWidth; ++ox) {
                const int x0 = static_cast<int>(ox * scaleX);
                const int x1 = std::min(width_, static_cast<int>((ox + 1) * scaleX));
                const int xEnd = std::max(x0 + 1, x1);
                for (int c = 0; c < ch; ++c) {
                    double sum = 0.0;
                    int n = 0;
                    for (int x = x0; x < xEnd; ++x) {
                        sum += inRow[static_cast<size_t>(x) * ch + c];
                        ++n;
                    }
                    outRow[static_cast<size_t>(ox) * ch + c] =
                        static_cast<float>(sum / std::max(1, n));
                }
            }
            writeRowFloats(*hpass, y, outRow);
        }
    });

    // Vertical pass straight into the target format (each output row reads a
    // small strip of the horizontal-pass image, so rows are independent).
    auto out = ImageBuffer::create(newWidth, newHeight, format);
    lrc::parallel::parallelFor(static_cast<size_t>(newHeight), rowGrain,
                               [&](size_t by, size_t ey) {
        std::vector<float> vRow(static_cast<size_t>(newWidth) * ch);
        std::vector<float> rowBuf;
        for (int oy = static_cast<int>(by); oy < static_cast<int>(ey); ++oy) {
            const int y0 = static_cast<int>(oy * scaleY);
            const int y1 = std::min(height_, static_cast<int>((oy + 1) * scaleY));
            const int yEnd = std::max(y0 + 1, y1);
            std::fill(vRow.begin(), vRow.end(), 0.0f);
            int n = 0;
            for (int y = y0; y < yEnd; ++y) {
                loadRowFloats(*hpass, y, rowBuf);
                for (size_t i = 0; i < vRow.size(); ++i)
                    vRow[i] += rowBuf[i];
                ++n;
            }
            const float inv = 1.0f / std::max(1, n);
            for (float& v : vRow)
                v *= inv;
            writeRowFloats(*out, oy, vRow);
        }
    });
    return out;
}

std::shared_ptr<ImageBuffer> ImageBuffer::toF32() const {
    auto out = ImageBuffer::create(width_, height_, PixelFormat::F32);
    // Each source row maps to exactly one output row, so rows can be converted
    // independently across the thread pool. Every chunk owns its row buffer --
    // scratch is never shared between chunks.
    const size_t grain =
        lrc::parallel::defaultGrain(static_cast<size_t>(height_));
    lrc::parallel::parallelFor(static_cast<size_t>(height_), grain,
                               [&](size_t by, size_t ey) {
        std::vector<float> row;
        for (int y = static_cast<int>(by); y < static_cast<int>(ey); ++y) {
            loadRowFloats(*this, y, row);
            writeRowFloats(*out, y, row);
        }
    });
    return out;
}

std::shared_ptr<ImageBuffer> ImageBuffer::to8bitSRGB(
    const ColorManager& cm) const {
    return cm.toDisplaySRGB(*this);
}

} // namespace lrc
