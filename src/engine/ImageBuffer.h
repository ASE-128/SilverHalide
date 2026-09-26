#pragma once

// Pure C++ image buffer shared by the engine and catalog layers.
// Strictly Qt-free. Uses RGB interleaved pixel data only (3 channels).

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

namespace lrc {

// Forward declaration: ColorManager lives in engine/ColorManager.h.
class ColorManager;

/**
 * @brief Pixel storage format of an ImageBuffer.
 *
 * Enum values select the per-channel storage width and layout. U16 and F32
 * are linear working spaces; U8 is display-ready (sRGB encoded).
 */
enum class PixelFormat : uint8_t {
    U16,   // linear, 16-bit per channel, normalized 0..65535
    F32,   // linear, 32-bit float per channel, normalized 0..1
    U8,    // 8-bit per channel, display-ready (sRGB encoded)
};

/**
 * @brief A 2D RGB image buffer shared by the engine and catalog layers.
 *
 * All pixel data is interleaved R,G,B per pixel. Strictly Qt-free. Buffers are
 * owned via ImageBufferPtr (shared_ptr); this type itself is not thread-safe,
 * so distinct buffers (or same-buffer access from one thread) must not be
 * mutated concurrently.
 */
class ImageBuffer {
public:
    ImageBuffer() = default;
    ImageBuffer(int width, int height, PixelFormat format);

    static std::shared_ptr<ImageBuffer> create(int width, int height,
                                               PixelFormat format);

    // --- metadata -------------------------------------------------------
    int width() const { return width_; }
    int height() const { return height_; }
    int channels() const { return channels_; }
    PixelFormat format() const { return format_; }
    bool empty() const { return width_ == 0 || height_ == 0; }
    size_t sizeBytes() const { return data_.size(); }

    std::vector<uint8_t>& data() { return data_; }
    const std::vector<uint8_t>& data() const { return data_; }

    // --- normalized pixel access ----------------------------------------
    /**
     * @brief Returns one channel value normalized to [0,1].
     *
     * U16 and U8 are divided by their range; F32 is returned as stored.
     *
     * @param x Column index in [0, width).
     * @param y Row index in [0, height).
     * @param channel Channel index in [0, channels).
     * @return Normalized value, or 0.0 for out-of-range coordinates.
     */
    float pixel(int x, int y, int channel) const;

    /**
     * @brief Writes a normalized [0,1] value into one channel.
     *
     * Same coordinate constraints as `pixel`; out-of-range writes are ignored.
     */
    void setPixel(int x, int y, int channel, float value);

    // --- operations returning new buffers --------------------------------
    std::shared_ptr<ImageBuffer> crop(int x, int y, int width,
                                      int height) const;

    /**
     * @brief Area-average downscale / nearest upscale.
     *
     * The safe path for previews. Preserves the source PixelFormat.
     */
    std::shared_ptr<ImageBuffer> resize(int newWidth, int newHeight) const;

    /**
     * @brief Area-average downscale into a float buffer.
     *
     * Like `resize`, but the output is always F32 even when the source is an
     * integer format - the preview pipeline renders in F32 and this avoids the
     * precision-losing F32->U16->F32 roundtrip that "resize then toF32" would
     * incur for a U16 source.
     */
    std::shared_ptr<ImageBuffer> resizeToF32(int newWidth, int newHeight) const;

    /**
     * @brief Converts to a display-ready U8 sRGB buffer.
     *
     * @param cm Color manager used for the colour transform.
     * @return New U8 sRGB buffer.
     */
    std::shared_ptr<ImageBuffer> to8bitSRGB(const ColorManager& cm) const;

    /**
     * @brief Converts this buffer to an equal-sized F32 buffer.
     */
    std::shared_ptr<ImageBuffer> toF32() const;

    /**
     * @brief Converts this buffer to an equal-sized U16 buffer.
     *
     * F32/U8 channel values are clamped to [0,1] and scaled to the U16
     * range; an already-U16 source is copied unchanged.
     */
    std::shared_ptr<ImageBuffer> toU16() const;

private:
    // Shared area-average core; `format` selects the output storage type.
    std::shared_ptr<ImageBuffer> resizeToFormat(int newWidth, int newHeight,
                                                PixelFormat format) const;
    int width_ = 0;
    int height_ = 0;
    int channels_ = 3;
    PixelFormat format_ = PixelFormat::F32;
    std::vector<uint8_t> data_;
};

using ImageBufferPtr = std::shared_ptr<ImageBuffer>;

} // namespace lrc
