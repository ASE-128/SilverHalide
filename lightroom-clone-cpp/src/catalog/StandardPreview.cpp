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
//   [ 0.. 4)  "SHSP"  magic
//   [ 4.. 8)  uint32  version (== 1)
//   [ 8..12)  uint32  width
//   [12..16)  uint32  height
//   [16..20)  uint32  channels (== 3)
//   [20..24)  uint32  format   (== 2, PixelFormat::F32)
//   [24.. )   width*height*3 little-endian float32 samples
constexpr size_t kHeaderBytes = 24;

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
        return nullptr;

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
        return nullptr;
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
    return f32;
}

std::vector<uint8_t> encodeStandardPreview(const ImageBuffer& buf) {
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
    out.reserve(kHeaderBytes + dataBytes);

    const char magic[4] = {'S', 'H', 'S', 'P'};
    out.insert(out.end(), magic, magic + 4);
    appendU32(out, 1u); // version
    appendU32(out, w);
    appendU32(out, h);
    appendU32(out, 3u); // channels
    appendU32(out, 2u); // PixelFormat::F32
    const uint8_t* src = buf.data().data();
    out.insert(out.end(), src, src + dataBytes);
    return out;
}

std::shared_ptr<ImageBuffer> decodeStandardPreview(
    const std::vector<uint8_t>& blob) {
    if (blob.size() < kHeaderBytes)
        return nullptr;
    if (blob[0] != 'S' || blob[1] != 'H' || blob[2] != 'S' || blob[3] != 'P')
        return nullptr;

    uint32_t version = 0, w = 0, h = 0, channels = 0, format = 0;
    if (!readU32(blob, 4, version) || !readU32(blob, 8, w) ||
        !readU32(blob, 12, h) || !readU32(blob, 16, channels) ||
        !readU32(blob, 20, format))
        return nullptr;
    if (version != 1 || w == 0 || h == 0 || channels != 3 || format != 2)
        return nullptr;
    // A standard preview never exceeds kStandardPreviewMaxDim; reject absurd
    // dimensions from a corrupted blob before they reach the allocator.
    if (w > 8192u || h > 8192u)
        return nullptr;

    const size_t expected =
        kHeaderBytes + static_cast<size_t>(w) * h * 3 * sizeof(float);
    if (blob.size() != expected)
        return nullptr;

    auto out = ImageBuffer::create(static_cast<int>(w), static_cast<int>(h),
                                   PixelFormat::F32);
    std::memcpy(out->data().data(), blob.data() + kHeaderBytes,
                blob.size() - kHeaderBytes);
    return out;
}

} // namespace lrc
