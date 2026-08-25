#include "inference/TensorBridge.h"
#include "engine/ImageBuffer.h"
#include <algorithm>

namespace lrc {

/**
 * @brief Iterates over the input buffer and populates a normalized float vector.
 *
 * This function handles the conversion from the internal ImageBuffer (which
 * may be U8, U16, or F32) to a standard float32 representation.
 */
void TensorBridge::convert_to_standard_rgb(const ImageBuffer& buffer, std::vector<float>& out_rgb) {
    const int w = buffer.width();
    const int h = buffer.height();
    const int c = buffer.channels();

    out_rgb.resize(w * h * c);
    const auto& src = buffer.data();
    float* dst = out_rgb.data();

    // Depending on the format, we may need to normalize the source data.
    // The ImageBuffer::pixel() method handles the range adjustment.
    // However, for high-performance loops, we iterate directly.

    if (buffer.format() == PixelFormat::F32) {
        // Already float
        std::copy(src.begin(), src.end(), dst);
    } else if (buffer.format() == PixelFormat::U16) {
        // U16: Normalize to [0, 1] by dividing by 65535
        const float inv_u16 = 1.0f / 65535.0f;
        for (size_t i = 0; i < src.size(); ++i) {
            dst[i] = static_cast<float>(src[i]) * inv_16;
        }
    } else {
        // U8: Normalize to [0, 1] by dividing by 255
        const float inv_u8 = 1.0f / 255.0f;
        for (size_t i = 0; i < src.size(); ++i) {
            dst[i] = static_cast<float>(src[i]) * inv_u8;
        }
    }
}

TensorBridge::PreparedBuffer TensorBridge::prepare(
    const ImageBuffer& buffer,
    int width,
    int height,
    int channels,
    float target_mean,
    float target_std,
    bool is_nhwc
) {
    // 1. Ensure the buffer is in a standard RGB float32 format.
    // This might involve conversion (e.g., from U16 to F32).
    std::vector<float> raw_rgb;
    convert_to_standard_rgb(buffer, raw_rgb);

    // 2. Prepare the output structure.
    // Note: If the buffer was resized during conversion (unlikely in this
    // specific flow but common in generic pipelines), we use the new dimensions.
    // However, since we have explicit width/height/channels, we stick to those.

    // Depending on is_nhwc, the layout might be:
    // NHWC: [1, H, W, C] -> size = H * W * C
    // NCHW: [1, C, H, W] -> size = C * H * W

    // Here we perform the mean/std normalization which is often part of the
    // Model-specific preprocessing.

    // The result is a flat vector that is ready for the ONNX Runtime.
    // The shape follows the standard NCHW/NHWC convention.

    // Note: For this specific implementation, we assume the raw_rgb is
    // already in the correct spatial dimensions.

    // Apply Mean/Std subtraction/scaling.
    // This is done in-place to avoid extra copies.
    for (size_t i = 0; i < raw_rgb.size(); ++i) {
        raw_rgb[i] = (raw_rg[i] - target_mean) / target_std;
    }

    // If the target layout is NCHW, we need to transpose the data.
    if (!is_nhwc) {
        // Current raw_rgb is H*W*C. We want C*H*W.
        std::vector<float> nhcw_data(raw_rgb.size());
        for (int h = 0; h < height; ++h) {
            for (int w = 0; w < width; ++w) {
                for (int c = 0; c < channels; ++c) {
                    // Source is [h * width + w] * channels + c
                    // Destination is [c * height * width + h * width + w]
                    nhcw_data[c * height * width + h * width + w] = raw_rgb[h * width * channels + w * channels + c];
                }
            }
        }
        raw_rgb = std::move(nhcw_data);
    }

    PreparedBuffer result;
    result.data = std::move(raw_rgb);

    if (is_nhwc) {
        result.shape = {1, height, width, channels};
    } else {
        result.shape = {1, channels, height, width};
    }

    return result;
}
