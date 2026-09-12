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
//   v3 (40 bytes): v2 + sourceColor + colorVersion. `sourceColor` is a stable
//                  hash of the source ICC (or 0 for untagged-sRGB / RAW) used
//                  to build the preview, and `colorVersion` is the colour
//                  conversion strategy version. `decodeStandardPreview` only
//                  accepts v3: a v1/v2 blob was produced before ICC-aware
//                  conversion (R-08/R-09) and is therefore treated as stale and
//                  regenerated rather than reusing possibly-wrong colours
//                  (R-10).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/ImageBuffer.h"

namespace lrc {

// Default longest edge (pixels) of a standard preview.
constexpr int kStandardPreviewMaxDim = 2048;

// Current source-colour conversion strategy recorded in the v3 header. Bump
// this together with the blob version when a conversion-policy change must
// invalidate already-cached previews. (Currently 1 = ICC-aware non-RAW import,
// R-08/R-09.)
constexpr uint32_t kStandardPreviewColorVersion = 1;

// Decodes `filePath` (RAW/DNG through libraw, JPEG/PNG/TIFF through stb_image +
// the color manager) into linear ProPhoto RGB F32, applies the default (zero)
// adjustment stack, and area-averages down to a kStandardPreviewMaxDim longest
// edge. Returns null on any decode/render failure; the caller then falls back
// to a full RAW decode (matching pre-standard-preview behaviour).
std::shared_ptr<ImageBuffer> buildStandardPreview(const std::string& filePath);

// S8a: same as buildStandardPreview but also reports the source's native
// full-resolution pixel dimensions (from the RAW/sRGB decoder metadata) and the
// source-colour hash, so the blob encoder can store them in the header. Kept as
// a separate entry point (instead of overloading buildStandardPreview) so
// existing callers stay unchanged.
struct BuiltStandardPreview {
    ImageBufferPtr buf;
    int nativeWidth = 0;   // full-resolution native width (0 when unknown)
    int nativeHeight = 0;  // full-resolution native height
    // Stable hash of the source ICC that produced `buf` (0 = untagged sRGB or
    // RAW); recorded in the v3 blob header so a stale colour disposition isn't
    // silently reused (R-10).
    uint32_t sourceColor = 0;
};
BuiltStandardPreview buildStandardPreview2(const std::string& filePath);

// Serializes a linear ProPhoto F32 RGB buffer to the catalog blob form. All
// overloads write the current v3 header (40 bytes):
// - Single-arg overload records sourceColor=0 and native 0 (legacy callers /
//   buffers without colour or dimension info).
// - Three/four-arg overloads carry native dimensions; the four-arg form also
//   records `sourceColor`.
// Returns an empty vector when the buffer is empty or not F32 RGB.
std::vector<uint8_t> encodeStandardPreview(const ImageBuffer& buf);
std::vector<uint8_t> encodeStandardPreview(const ImageBuffer& buf,
                                           int nativeWidth, int nativeHeight);
std::vector<uint8_t> encodeStandardPreview(const ImageBuffer& buf,
                                           int nativeWidth, int nativeHeight,
                                           uint32_t sourceColor);

// S8a: result of decoding a catalog blob, carrying the native dimensions so
// the preview pipeline can scale its spatial kernels correctly. native{W,H}
// is 0 when unknown.
//
// R-10: only v3 blobs are accepted. A v1/v2 blob (pre-ICC-aware conversion) is
// treated as stale and returns a null buffer so the caller regenerates it under
// the current colour policy. `sourceColor`/`colorVersion` expose what produced
// the pixels.
struct DecodedStandardPreview {
    ImageBufferPtr buf;
    int nativeWidth = 0;
    int nativeHeight = 0;
    uint32_t sourceColor = 0;
    uint32_t colorVersion = 0;
};

// Deserializes the catalog blob form back to a linear ProPhoto F32 buffer.
// Only the current v3 format is accepted (see the version notes above). Returns
// a null buffer (buf == nullptr) when the blob is malformed, truncated, the
// wrong format, or an obsolete v1/v2 blob, so stale records are regenerated
// rather than reused (R-10). Never crashes the caller.
DecodedStandardPreview decodeStandardPreview(
    const std::vector<uint8_t>& blob);

} // namespace lrc
