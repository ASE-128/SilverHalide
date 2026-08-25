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

enum class PixelFormat : uint8_t {
    U16,   // linear, 16-bit per channel, normalized 0..65535
    F32,   // linear, 32-bit float per channel, normalized 0..1
    U8,    // 8-bit per channel, display-ready (sRGB encoded)
};

// A 2D RGB image buffer. All pixel data is interleaved: R,G,B per pixel.
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
    // Returns the channel value normalized to [0,1]. U16 and U8 are
    // divided by their range; F32 is returned as stored.
    float pixel(int x, int y, int channel) const;
    // Writes a normalized [0,1] value into the given channel.
    void setPixel(int x, int y, int channel, float value);

    // --- operations returning new buffers --------------------------------
    std::shared_ptr<ImageBuffer> crop(int x, int y, int width,
                                      int height) const;
    // Area-average downscale / nearest upscale. The safe path for previews.
    std::shared_ptr<ImageBuffer> resize(int newWidth, int newHeight) const;
    // Area-average downscale into a float buffer. Like resize, but the output
    // is always F32 even when the source is an integer format -- the preview
    // pipeline renders in F32 and this avoids the precision-losing
    // F32->U16->F32 roundtrip that "resize then toF32" would incur for a U16
    // source.
    std::shared_ptr<ImageBuffer> resizeToF32(int newWidth, int newHeight) const;
    // Converts to a display-ready U8 sRGB buffer through the color manager.
    std::shared_ptr<ImageBuffer> to8bitSRGB(const ColorManager& cm) const;

    // Convert to a float buffer (allocates F32 of the same size).
    std::shared_ptr<ImageBuffer> toF32() const;

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
