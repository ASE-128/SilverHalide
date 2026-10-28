#include "engine/CropTransform.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lrc {
namespace {

float sampleBilinear(std::span<const float> pixels, int width, int height,
                     double x, double y, int channel) noexcept {
    if (x < 0.0 || y < 0.0 || x > width - 1.0 || y > height - 1.0)
        return 0.0f;
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const double tx = x - x0;
    const double ty = y - y0;
    const auto at = [width, channel](int px, int py) {
        return static_cast<size_t>((py * width + px) * 3 + channel);
    };
    const double top = pixels[at(x0, y0)] * (1.0 - tx) +
                       pixels[at(x1, y0)] * tx;
    const double bottom = pixels[at(x0, y1)] * (1.0 - tx) +
                          pixels[at(x1, y1)] * tx;
    return static_cast<float>(top * (1.0 - ty) + bottom * ty);
}

} // namespace

bool hasCropTransform(const CropRect& crop, double angleDegrees) noexcept {
    return crop != CropRect{} || angleDegrees != 0.0;
}

ImageBufferPtr applyCropAndStraighten(const ImageBuffer& source,
                                      const CropRect& crop,
                                      double angleDegrees) {
    if (source.empty())
        return ImageBuffer::create(0, 0, PixelFormat::F32);
    if (source.format() != PixelFormat::F32)
        return applyCropAndStraighten(*source.toF32(), crop, angleDegrees);
    if (!std::isfinite(crop.left) || !std::isfinite(crop.top) ||
        !std::isfinite(crop.right) || !std::isfinite(crop.bottom) ||
        !std::isfinite(angleDegrees) || crop.left < 0.0f || crop.top < 0.0f ||
        crop.right > 1.0f || crop.bottom > 1.0f ||
        crop.right <= crop.left || crop.bottom <= crop.top ||
        angleDegrees < -45.0 || angleDegrees > 45.0) {
        return std::make_shared<ImageBuffer>(source);
    }

    const int outputWidth = std::max(
        1, static_cast<int>(std::lround(source.width() *
                                        (crop.right - crop.left))));
    const int outputHeight = std::max(
        1, static_cast<int>(std::lround(source.height() *
                                        (crop.bottom - crop.top))));
    auto output =
        ImageBuffer::create(outputWidth, outputHeight, PixelFormat::F32);
    const std::span<const float> input = source.asFloatSpan();
    const std::span<float> destination = output->asMutableFloatSpan();
    const double radians = angleDegrees * std::numbers::pi / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const double centerX = (source.width() - 1.0) * 0.5;
    const double centerY = (source.height() - 1.0) * 0.5;

    for (int y = 0; y < outputHeight; ++y) {
        const double rotatedY =
            (crop.top + (y + 0.5) / outputHeight * (crop.bottom - crop.top)) *
                source.height() -
            0.5;
        for (int x = 0; x < outputWidth; ++x) {
            const double rotatedX =
                (crop.left + (x + 0.5) / outputWidth *
                                 (crop.right - crop.left)) *
                    source.width() -
                0.5;
            const double dx = rotatedX - centerX;
            const double dy = rotatedY - centerY;
            const double sourceX = centerX + cosine * dx + sine * dy;
            const double sourceY = centerY - sine * dx + cosine * dy;
            const size_t offset =
                static_cast<size_t>((y * outputWidth + x) * 3);
            for (int channel = 0; channel < 3; ++channel)
                destination[offset + channel] = sampleBilinear(
                    input, source.width(), source.height(), sourceX, sourceY,
                    channel);
        }
    }
    return output;
}

} // namespace lrc
