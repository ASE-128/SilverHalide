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
//
// Blob header versions:
//   v1 (24 bytes): magic + version + w + h + channels + format.
//   v2 (32 bytes): v1 + nativeWidth + nativeHeight. The native dimensions let
//                  the preview's spatial-kernel sigma scaling use the true
//                  full-resolution edge (Pipeline::previewSigmaScale) instead
//                  of a rough 2x estimate.

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

// S8a: same as buildStandardPreview but also reports the source's native
// full-resolution pixel dimensions (from the RAW/sRGB decoder metadata) so
// the blob encoder can store them in the v2 header. Kept as a separate entry
// point (instead of overloading buildStandardPreview) so existing callers stay
// unchanged.
struct BuiltStandardPreview {
    ImageBufferPtr buf;
    int nativeWidth = 0;   // full-resolution native width (0 when unknown)
    int nativeHeight = 0;  // full-resolution native height
};
BuiltStandardPreview buildStandardPreview2(const std::string& filePath);

// Serializes a linear ProPhoto F32 RGB buffer to the catalog blob form.
// - Single-arg overload writes the legacy v1 header (24 bytes). Kept for
//   existing callers that do not yet know the native dimensions.
// - Three-arg overload writes the v2 header (32 bytes) carrying nativeW/H.
// Returns an empty vector when the buffer is empty or not F32 RGB.
std::vector<uint8_t> encodeStandardPreview(const ImageBuffer& buf);
std::vector<uint8_t> encodeStandardPreview(const ImageBuffer& buf,
                                           int nativeWidth, int nativeHeight);

// S8a: result of decoding a catalog blob, carrying the native dimensions so
// the preview pipeline can scale its spatial kernels correctly. native{W,H}
// is 0 for v1 blobs (caller treats 0 as "unknown" and falls back to an
// estimate).
struct DecodedStandardPreview {
    ImageBufferPtr buf;
    int nativeWidth = 0;
    int nativeHeight = 0;
};

// Deserializes the catalog blob form back to a linear ProPhoto F32 buffer.
// Accepts both v1 (24-byte header) and v2 (32-byte header) blobs. Returns a
// null buffer (buf == nullptr) when the blob is malformed, truncated, or the
// wrong format, so old or corrupted records never crash the caller.
DecodedStandardPreview decodeStandardPreview(
    const std::vector<uint8_t>& blob);

} // namespace lrc
