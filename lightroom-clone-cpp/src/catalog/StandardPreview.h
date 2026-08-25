#pragma once

// Standard preview builder for the catalog layer. Pure C++ — no Qt.
//
// A standard preview is the Develop module's fit-view base layer: the photo
// decoded into linear ProPhoto RGB F32 and rendered at *default* (zero)
// adjustments, downscaled to a 2048px longest edge. It lets the Develop panel
// start from a cached render instead of a full-resolution RAW decode on every
// photo change (Lightroom's "Standard" preview).
//
// The catalog blob form is a small header + raw F32 RGB pixel data, so reading
// a stored preview back is a memcpy, never a re-render.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/ImageBuffer.h"

namespace lrc {

// Default longest edge (pixels) of a standard preview.
constexpr int kStandardPreviewMaxDim = 2048;

// Decodes `filePath` (RAW/DNG through libraw, JPEG/PNG/TIFF through stb_image +
// the color manager) into linear ProPhoto RGB F32, applies the default (zero)
// adjustment stack, and area-averages down to a kStandardPreviewMaxDim longest
// edge. Returns null on any decode/render failure; the caller then falls back
// to a full RAW decode (matching pre-standard-preview behaviour).
std::shared_ptr<ImageBuffer> buildStandardPreview(const std::string& filePath);

// Serializes a linear ProPhoto F32 RGB buffer to the catalog blob form.
// Returns an empty vector when the buffer is empty or not F32 RGB.
std::vector<uint8_t> encodeStandardPreview(const ImageBuffer& buf);

// Deserializes the catalog blob form back to a linear ProPhoto F32 buffer.
// Returns null when the blob is malformed, truncated, or the wrong format, so
// old or corrupted records never crash the caller.
std::shared_ptr<ImageBuffer> decodeStandardPreview(
    const std::vector<uint8_t>& blob);

} // namespace lrc
