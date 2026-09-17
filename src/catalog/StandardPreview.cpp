#include "catalog/StandardPreview.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <utility>

#include "engine/Adjustments.h"
#include "engine/ColorManager.h"
#include "engine/Pipeline.h"
#include "engine/RawDecoder.h"
#include "utils/MathUtils.h"

namespace lrc {

namespace {

// Blob layout (little-endian header, native-order float payload):
//   [ 0.. 4)   "SHSP"  magic
//   [ 4.. 8)   uint32  version (== 3)
//   [ 8..12)   uint32  width    (preview width)
//   [12..16)   uint32  height   (preview height)
//   [16..20)   uint32  channels (== 3)
//   [20..24)   uint32  format   (== 2, PixelFormat::F32)
//   [24..28)   uint32  nativeWidth
//   [28..32)   uint32  nativeHeight
//   [32..36)   uint32  sourceColor   (hash of source ICC; 0 = untagged sRGB/RAW)
//   [36..40)   uint32  colorVersion   (kStandardPreviewColorVersion)
//   [40..)     width*height*3 little-endian float32 samples
constexpr size_t kHeaderV3Bytes = 40;

void appendU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

bool readU32(const std::vector<uint8_t>& blob, size_t off, uint32_t& out) {
    if (off + 4 > blob.size())
        return false;
    out = static_cast<uint32_t>(blob[off]) |
          (static_cast<uint32_t>(blob[off + 1]) << 8) |
          (static_cast<uint32_t>(blob[off + 2]) << 16) |
          (static_cast<uint32_t>(blob[off + 3]) << 24);
    return true;
}

} // namespace

std::shared_ptr<ImageBuffer> buildStandardPreview(const std::string& filePath) {
    return buildStandardPreview2(filePath).buf;
}

BuiltStandardPreview buildStandardPreview2(const std::string& filePath) {
    // UTF-8 path -> filesystem::path through the UTF-16 layer, so Chinese
    // filenames survive on MSVC (see utils/MathUtils.h).
    const std::filesystem::path path = lrc::utf8ToPath(filePath);

    // Self-contained: buildStandardPreview has no catalog/UI context, so it
    // constructs the decoder + colour manager it needs. Each call builds a
    // fresh ColorManager (lcms transform construction is cheap and the
    // resulting transforms are internally thread-safe). The standard preview
    // stays in linear ProPhoto F32, so the display profile never influences it.
    auto cm = std::make_shared<ColorManager>();
    RawDecoder decoder;
    decoder.setColorManager(cm);

    // Decode at full resolution, bounded to 2x the target (oversample) so the
    // final area-average downscale stays clean. halfSize is deliberately not
    // used: a small RAW would otherwise be halved below the 2048px target.
    DecodeParams params;
    params.maxDim = kStandardPreviewMaxDim * 2;
    auto decoded = decoder.decode(path, params);
    if (!decoded || !decoded->linear || decoded->linear->empty())
        return {};

    // Record the native full-resolution dimensions before the downscale below
    // throws them away. The decoder fills nativeWidth/Height from the RAW
    // metadata (or the source image size for non-RAW files); either way this
    // is what Pipeline::previewSigmaScale needs to scale spatial kernels.
    const int nativeW = decoded->meta.nativeWidth;
    const int nativeH = decoded->meta.nativeHeight;

    // Work in F32 linear ProPhoto. The decode buffer is owned by this call, so
    // it can be edited in place (the pipeline's in-place apply is safe).
    ImageBufferPtr f32;
    if (decoded->linear->format() == PixelFormat::F32)
        f32 = std::move(decoded->linear);
    else
        f32 = decoded->linear->toF32();

    const int w = f32->width();
    const int h = f32->height();
    if (w <= 0 || h <= 0)
        return {};
    float* data = reinterpret_cast<float*>(f32->data().data());
    const size_t pixelCount = static_cast<size_t>(w) * h;

    // Default (zero) adjustments: as-shot white balance (already applied at
    // decode) with identity exposure/contrast/curve/spatial. These calls mirror
    // Pipeline::renderInternalLinear so the standard preview is exactly what the
    // pipeline produces at default settings.
    Pipeline::applyAdjustments(Adjustments{}, data, pixelCount);
    Pipeline::applySpatialAdjustments(Adjustments{}, data, w, h);

    const int longest = std::max(w, h);
    if (longest > kStandardPreviewMaxDim) {
        const double scale =
            static_cast<double>(kStandardPreviewMaxDim) / longest;
        const int nw = std::max(1, static_cast<int>(w * scale));
        const int nh = std::max(1, static_cast<int>(h * scale));
        f32 = f32->resize(nw, nh);
    }
    return {f32, nativeW, nativeH, decoded->sourceColorHash};
}

static void encodeHeaderV3(std::vector<uint8_t>& out,
                           uint32_t w, uint32_t h,
                           uint32_t nativeW, uint32_t nativeH,
                           uint32_t sourceColor) {
    const char magic[4] = {'S', 'H', 'S', 'P'};
    out.insert(out.end(), magic, magic + 4);
    appendU32(out, 3u);  // version
    appendU32(out, w);
    appendU32(out, h);
    appendU32(out, 3u);  // channels
    appendU32(out, 2u);  // PixelFormat::F32
    appendU32(out, nativeW);
    appendU32(out, nativeH);
    appendU32(out, sourceColor);
    appendU32(out, kStandardPreviewColorVersion);
}

std::vector<uint8_t> encodeStandardPreview(const ImageBuffer& buf) {
    // Legacy single-arg overload: no native dims / colour info available.
    return encodeStandardPreview(buf, 0, 0, 0);
}

std::vector<uint8_t> encodeStandardPreview(const ImageBuffer& buf,
                                           int nativeWidth,
                                           int nativeHeight) {
    return encodeStandardPreview(buf, nativeWidth, nativeHeight, 0);
}

std::vector<uint8_t> encodeStandardPreview(const ImageBuffer& buf,
                                           int nativeWidth,
                                           int nativeHeight,
                                           uint32_t sourceColor) {
    // Preferred v3 encoder: records native dimensions and the source colour
    // hash so a later policy change invalidates the cache and never reuses
    // wrong-coloured pixels.
    std::vector<uint8_t> out;
    if (buf.empty() || buf.format() != PixelFormat::F32 ||
        buf.channels() != 3)
        return out;
    const uint32_t w = static_cast<uint32_t>(buf.width());
    const uint32_t h = static_cast<uint32_t>(buf.height());
    if (w == 0 || h == 0)
        return out;

    const size_t dataBytes =
        static_cast<size_t>(w) * h * 3 * sizeof(float);
    out.reserve(kHeaderV3Bytes + dataBytes);
    encodeHeaderV3(out, w, h,
                   static_cast<uint32_t>(nativeWidth > 0 ? nativeWidth : 0),
                   static_cast<uint32_t>(nativeHeight > 0 ? nativeHeight : 0),
                   sourceColor);
    const uint8_t* src = buf.data().data();
    out.insert(out.end(), src, src + dataBytes);
    return out;
}

DecodedStandardPreview decodeStandardPreview(
    const std::vector<uint8_t>& blob) {
    if (blob.size() < kHeaderV3Bytes)
        return {};
    if (blob[0] != 'S' || blob[1] != 'H' || blob[2] != 'S' || blob[3] != 'P')
        return {};

    uint32_t version = 0, w = 0, h = 0, channels = 0, format = 0;
    uint32_t nativeW = 0, nativeH = 0, sourceColor = 0, colorVersion = 0;
    if (!readU32(blob, 4, version) || !readU32(blob, 8, w) ||
        !readU32(blob, 12, h) || !readU32(blob, 16, channels) ||
        !readU32(blob, 20, format) || !readU32(blob, 24, nativeW) ||
        !readU32(blob, 28, nativeH) || !readU32(blob, 32, sourceColor) ||
        !readU32(blob, 36, colorVersion))
        return {};
    // Only the current v3 blob is accepted. An obsolete v1/v2 blob was produced
    // before ICC-aware conversion; returning {} here makes the
    // caller regenerate it under the current colour policy instead of reusing
    // potentially wrong-coloured pixels.
    if (version != 3 || w == 0 || h == 0 || channels != 3 || format != 2)
        return {};
    // A standard preview never exceeds kStandardPreviewMaxDim; reject absurd
    // dimensions from a corrupted blob before they reach the allocator.
    if (w > 8192u || h > 8192u)
        return {};

    const size_t expected =
        kHeaderV3Bytes + static_cast<size_t>(w) * h * 3 * sizeof(float);
    if (blob.size() != expected)
        return {};

    auto out = ImageBuffer::create(static_cast<int>(w), static_cast<int>(h),
                                   PixelFormat::F32);
    std::memcpy(out->data().data(), blob.data() + kHeaderV3Bytes,
                blob.size() - kHeaderV3Bytes);
    return {out, static_cast<int>(nativeW), static_cast<int>(nativeH),
            sourceColor, colorVersion};
}

} // namespace lrc
