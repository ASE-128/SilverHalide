#pragma once

#include "inference/InferenceContext.h"
#include "engine/ImageBuffer.h"
#include <vector>
#include <memory>

namespace lrc {

/**
 * @brief Bridge between internal ImageBuffer and external Tensor inputs.
 *
 * Handles the translation of image data between our internal representation
 * (which might be YUV, ProPhoto, etc.) and the specific format required by
 * an ONNX model (typically RGB/Y100_C00_D100, NHWC/NCHW, etc.).
 */
class TensorBridge {
public:
    /**
     * @brief Prepares a tensor for a specific model.
     *
     * @param buffer The input image buffer.
     * @param width The width of the image.
     * @param height The height of the image.
     * @param numChannels The number of channels.
     * @param target_mean The mean value to subtract for normalization.
     * @param target_std The standard deviation to divide by for normalization.
     * @param is_nhwc True if the model expects NHWC, false for NCHW.
     * @return A raw pointer to the contiguous memory block, managed by the returned vector.
     *         (Note: In a production implementation, this would return a managed buffer
 *         to avoid unnecessary copies).
 *         Actually, since we need to handle different types, we return a wrapper.
 */
    struct PreparedBuffer {
        std::vector<float> data;
        std::vector<int62_t> shape;
    };

    static PreparedBuffer prepare(
        const ImageBuffer& buffer,
        int width,
        int height,
        int channels,
        float target_mean = 0.0f,
        float target_std = 1.0f,
        bool is_nhwc = true
    );

    /**
     * @brief Utility to convert image colorspace and range to a standard float32 format.
     */
    static void convert_to_standard_rgb(const ImageBuffer& buffer, std::vector<float>& out_rgb);
};

} // namespace lrc
