#pragma once

// Pure C++ image buffer shared by the engine and catalog layers.
// Strictly Qt-free. Uses RGB interleaved pixel data only (3 channels).

#include <cstdint>
#include <cstddef>
#include <memory>
#include <span>
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
    /**
     * @brief Allocates an RGB buffer of `width` x `height` pixels.
     *
     * @param width  Pixel width; must be >= 0.
     * @param height Pixel height; must be >= 0.
     * @param format Pixel format (U16/F32/U8), which fixes the byte depth.
     *
     * @warning This constructor does not have a failure return: it throws
     *          std::invalid_argument for a negative dimension and
     *          std::length_error / std::bad_alloc when the pixel data cannot
     *          be allocated. Callers that must not propagate exceptions (Qt
     *          slots, decoders fed by untrusted files) have to validate the
     *          dimensions against their own limits and wrap the call in a
     *          try/catch -- do not test the result for nullptr.
     */
    ImageBuffer(int width, int height, PixelFormat format);

    /**
     * @brief Factory for a shared RGB buffer.
     *
     * @note Never returns nullptr: allocation failures and invalid dimensions
     *       throw exactly as the constructor does (see above). The returned
     *       pointer is non-null even for a 0 x 0 request.
     */
    [[nodiscard]] static std::shared_ptr<ImageBuffer> create(
        int width, int height, PixelFormat format);

    // --- metadata -------------------------------------------------------
    /**
     * @brief Image metadata and raw storage accessors; none of them throw.
     *
     * width()/height() are the pixel dimensions, channels() is always 3, and
     * empty() is true when either dimension is 0. sizeBytes() is the byte size
     * of the underlying allocation (the exact extent of a well-formed buffer).
     * The two data() overloads expose that same storage for serialization and
     * bulk copies; they alias the bytes the typed views and pixel() read.
     *
     * @note All of these are noexcept: they only read member state, so they
     *       are safe to call from a destructor or exception path.
     */
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    int channels() const noexcept { return channels_; }
    PixelFormat format() const noexcept { return format_; }
    bool empty() const noexcept { return width_ == 0 || height_ == 0; }
    size_t sizeBytes() const noexcept { return data_.size(); }

    std::vector<uint8_t>& data() noexcept { return data_; }
    const std::vector<uint8_t>& data() const noexcept { return data_; }

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
     *
     * @note Never throws: it only reads this buffer, so out-of-range input is
     *       reported as 0.0 instead of an exception.
     */
    float pixel(int x, int y, int channel) const noexcept;

    /**
     * @brief Writes a normalized [0,1] value into one channel.
     *
     * Same coordinate constraints as `pixel`; out-of-range writes are ignored.
     *
     * @note Never throws: this is a plain store into the existing allocation
     *       (coordinates outside the buffer are dropped), so it cannot fail
     *       with std::out_of_range / std::bad_alloc.
     */
    void setPixel(int x, int y, int channel, float value) noexcept;

    // --- typed F32 views -------------------------------------------------
    /**
     * @brief Read-only typed view over the interleaved F32 pixel storage.
     *
     * The span aliases the same bytes as data(), reinterpreted as float, with
     * length width() * height() * channels(). Element index
     * (y * width() + x) * channels() + c addresses exactly the value that
     * pixel(x, y, c) returns, so a hot loop can hoist the format switch and
     * the per-pixel bounds checks out of its body.
     *
     * @return A view of the storage when format() == F32 and the buffer is
     *         non-empty; otherwise an empty span. This accessor never throws
     *         and never allocates, so non-F32 and empty inputs are reported
     *         through size() == 0 and the caller must check before indexing.
     *
     * @warning Non-owning view. It is invalidated by anything that
     *          reallocates or resizes the buffer (construction, assignment,
     *          data() mutation, conversion-producing operations) and must not
     *          outlive the ImageBuffer. Concurrent reads through several
     *          spans are fine as long as no thread mutates the buffer.
     * @note Storage type, byte layout and PixelFormat values are unchanged;
     *       this is a view, not a second representation.
     * @note Never throws: it only wraps the existing bytes in a span.
     */
    std::span<const float> asFloatSpan() const noexcept;

    /**
     * @brief Mutable typed view over the interleaved F32 pixel storage.
     *
     * Same layout and length contract as asFloatSpan(). Writing span[i] is
     * equivalent to setPixel(x, y, c, v) for the matching index, minus the
     * per-write format switch.
     *
     * @return A writable view when format() == F32 and the buffer is
     *         non-empty; otherwise an empty span (checked with size() == 0).
     *
     * @warning Requires exclusive access to this buffer: no other thread may
     *          read or write it while the span is in use. The view must not
     *          outlive the ImageBuffer or survive any reallocation.
     * @note Never throws: it only wraps the existing bytes in a span.
     */
    std::span<float> asMutableFloatSpan() noexcept;

    // --- operations returning new buffers --------------------------------
    [[nodiscard]] std::shared_ptr<ImageBuffer> crop(int x, int y, int width,
                                                    int height) const;

    /**
     * @brief Area-average downscale / nearest upscale.
     *
     * The safe path for previews. Preserves the source PixelFormat.
     */
    [[nodiscard]] std::shared_ptr<ImageBuffer> resize(int newWidth,
                                                     int newHeight) const;

    /**
     * @brief Area-average downscale into a float buffer.
     *
     * Like `resize`, but the output is always F32 even when the source is an
     * integer format - the preview pipeline renders in F32 and this avoids the
     * precision-losing F32->U16->F32 roundtrip that "resize then toF32" would
     * incur for a U16 source.
     */
    [[nodiscard]] std::shared_ptr<ImageBuffer> resizeToF32(
        int newWidth, int newHeight) const;

    /**
     * @brief Converts to a display-ready U8 sRGB buffer.
     *
     * @param cm Color manager used for the colour transform.
     * @return New U8 sRGB buffer.
     */
    [[nodiscard]] std::shared_ptr<ImageBuffer> to8bitSRGB(
        const ColorManager& cm) const;

    /**
     * @brief Converts this buffer to an equal-sized F32 buffer.
     */
    [[nodiscard]] std::shared_ptr<ImageBuffer> toF32() const;

    /**
     * @brief Converts this buffer to an equal-sized U16 buffer.
     *
     * F32/U8 channel values are clamped to [0,1] and scaled to the U16
     * range; an already-U16 source is copied unchanged.
     */
    [[nodiscard]] std::shared_ptr<ImageBuffer> toU16() const;

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
