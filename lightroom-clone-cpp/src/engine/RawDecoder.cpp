#include "engine/RawDecoder.h"

#include <algorithm>
#include <cstring>

#include <libraw/libraw.h>

#include "engine/ColorManager.h"
#include "engine/ImageFormats.h"
#include "engine/ImageIO.h"
#include "utils/Parallel.h"

namespace lrc {

namespace {

bool rawOk(const char* what, int ret) {
    return ret == LIBRAW_SUCCESS;
}

} // namespace

void RawDecoder::setColorManager(std::shared_ptr<const ColorManager> cm) {
    cm_ = std::move(cm);
}

bool RawDecoder::isSupported(const std::filesystem::path& path) {
    // Any extension the app can import is supported: RAW/DNG through libraw,
    // JPEG/PNG/TIFF through stb_image.
    return isImportablePath(path);
}

std::optional<DecodedImage> RawDecoder::decode(
    const std::filesystem::path& path, const DecodeParams& params) const {
    // Non-RAW formats (JPEG/PNG/TIFF) never reach libraw: they are already
    // demosaiced and rendered, so libraw would mis-treat them as camera data.
    if (!isRawPath(path))
        return decodeNonRaw(path, params);

    LibRaw raw;
    if (!rawOk("open", raw.open_file(path.string().c_str())))
        return std::nullopt;

    libraw_output_params_t& p = raw.imgdata.params;
    // output_color=4 lets libraw apply its own (well-tested) camera matrix +
    // white balance into a linear ProPhoto space. The direct cam_xyz manual
    // matrix path was removed: libraw's cam_xyz is the pre-white-balance matrix
    // and mixing it into the post-WB pixels produced badly wrong colours. The
    // D65 white point of libraw's ProPhoto is re-anchored to the working D50
    // ProPhoto by ColorManager::fromDcrawProPhoto below.
    p.output_color = 4;   // linear ProPhoto (D65 white), libraw's own matrix
    p.output_bps = 16;
    p.use_camera_wb = params.applyCameraWB ? 1 : 0;
    p.no_auto_bright = 1;
    // no_auto_scale must stay 0: libraw only runs scale_colors() (which applies
    // the camera white-balance gains in pre_mul) when auto-scale is NOT
    // suppressed. With no_auto_bright=1 the scale stays linear (raw/maximum),
    // so this is just "apply WB, no extra brightening" -- setting it to 1 here
    // dropped the WB entirely and every RAW came out green (the camera's R/B
    // gains were never multiplied in).
    p.no_auto_scale = 0;
    p.highlight = 0;      // clip, no highlight reconstruction
    p.gamm[0] = 1.0;
    p.gamm[1] = 1.0;      // keep a perfectly linear response
    p.half_size = params.halfSize ? 1 : 0;

    if (!rawOk("unpack", raw.unpack()))
        return std::nullopt;
    if (!rawOk("process", raw.dcraw_process()))
        return std::nullopt;

    libraw_processed_image_t* img = raw.dcraw_make_mem_image();
    if (!img || img->bits != 16 || (img->colors != 3 && img->colors != 4)) {
        if (img)
            libraw_dcraw_clear_mem(img);
        return std::nullopt;
    }

    RawMeta meta;
    meta.make = std::string(raw.imgdata.idata.make);
    meta.model = std::string(raw.imgdata.idata.model);
    meta.width = img->width;
    meta.height = img->height;
    // 原生尺寸 = 全尺寸解码 (halfSize=false) 的输出，含 90° 翻转换算 (native).
    // img->width 已被 half_size 减半，不能当原生尺寸 (img is halved by half_size).
    const bool rotated = (raw.imgdata.sizes.flip & 4) != 0;
    meta.nativeWidth = rotated ? raw.imgdata.sizes.iheight : raw.imgdata.sizes.iwidth;
    meta.nativeHeight = rotated ? raw.imgdata.sizes.iwidth : raw.imgdata.sizes.iheight;
    // As-shot white balance multipliers (relative to green).
    const double gm = raw.imgdata.color.cam_mul[1] > 0 ? raw.imgdata.color.cam_mul[1] : 1.0;
    meta.asShotWBR = raw.imgdata.color.cam_mul[0] / gm;
    meta.asShotWBG = 1.0;
    meta.asShotWBB = raw.imgdata.color.cam_mul[2] / gm;

    // Copy the decoded 16-bit RGB data into an ImageBuffer.
    auto linear = ImageBuffer::create(img->width, img->height, PixelFormat::U16);
    const size_t nPixels = static_cast<size_t>(img->width) * img->height;
    if (img->colors == 3) {
        const size_t bytes = nPixels * 3 * 2;
        std::memcpy(linear->data().data(), img->data, bytes);
    } else {
        // A DNG may spell a plain two-green Bayer CFA with the second-green
        // code (3) instead of two identical green codes (1). libraw then
        // counts four distinct CFA colors, treats the file as a 4-color
        // sensor, and (with output_color = 0) returns four channels in CFA
        // code order: R, G, B, G2. Average the two greens so the rest of the
        // pipeline sees the usual 3-channel linear RGB.
        const uint16_t* src = reinterpret_cast<const uint16_t*>(img->data);
        uint16_t* dst = reinterpret_cast<uint16_t*>(linear->data().data());
        lrc::parallel::parallelFor(
            nPixels, lrc::parallel::defaultGrain(nPixels),
            [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    dst[i * 3 + 0] = src[i * 4 + 0];
                    dst[i * 3 + 1] = static_cast<uint16_t>(
                        (static_cast<uint32_t>(src[i * 4 + 1]) +
                         src[i * 4 + 3]) /
                        2);
                    dst[i * 3 + 2] = src[i * 4 + 2];
                }
            });
    }
    libraw_dcraw_clear_mem(img);

    // Re-anchor libraw's D65-white ProPhoto into the working D50 ProPhoto.
    // Without a color manager (headless format tests) the buffer stays in
    // libraw's ProPhoto space; the GUI always injects one.
    if (cm_)
        linear = cm_->fromDcrawProPhoto(*linear);

    if (params.maxDim > 0) {
        const int longest = std::max(linear->width(), linear->height());
        if (longest > params.maxDim) {
            const double scale = static_cast<double>(params.maxDim) / longest;
            const int nw = std::max(1, static_cast<int>(linear->width() * scale));
            const int nh = std::max(1, static_cast<int>(linear->height() * scale));
            linear = linear->resize(nw, nh);
        }
    }
    meta.width = linear->width();
    meta.height = linear->height();

    return DecodedImage{std::move(linear), std::move(meta)};
}

std::optional<std::vector<uint8_t>> RawDecoder::extractEmbeddedThumbnail(
    const std::filesystem::path& path) const {
    LibRaw raw;
    if (!rawOk("open", raw.open_file(path.string().c_str())))
        return std::nullopt;
    raw.imgdata.params.output_bps = 16;
    raw.imgdata.params.use_camera_wb = 1;
    if (!rawOk("unpack_thumb", raw.unpack_thumb()))
        return std::nullopt;
    libraw_processed_image_t* thumb = raw.dcraw_make_mem_thumb();
    if (!thumb || thumb->type != LIBRAW_IMAGE_JPEG || thumb->data_size == 0) {
        if (thumb)
            libraw_dcraw_clear_mem(thumb);
        return std::nullopt;
    }
    std::vector<uint8_t> bytes(thumb->data, thumb->data + thumb->data_size);
    libraw_dcraw_clear_mem(thumb);
    return bytes;
}

std::optional<DecodedImage> RawDecoder::decodeNonRaw(
    const std::filesystem::path& path, const DecodeParams& params) const {
    if (!cm_)
        return std::nullopt;

    // stb_image reads JPEG/PNG/TIFF into a display-ready 8-bit sRGB buffer.
    auto srgb = ImageIO::loadImage(path);
    if (!srgb || srgb->empty())
        return std::nullopt;

    // Convert to the same linear ProPhoto working space the RAW path uses, so
    // the rest of the pipeline treats every import identically.
    auto linear = cm_->srgbToLinearProPhoto(*srgb);
    if (!linear || linear->empty())
        return std::nullopt;

    // Native size before the maxDim downscale below (native size).
    const int nativeW = linear->width();
    const int nativeH = linear->height();

    if (params.maxDim > 0) {
        const int longest = std::max(linear->width(), linear->height());
        if (longest > params.maxDim) {
            const double scale = static_cast<double>(params.maxDim) / longest;
            const int nw =
                std::max(1, static_cast<int>(linear->width() * scale));
            const int nh =
                std::max(1, static_cast<int>(linear->height() * scale));
            linear = linear->resize(nw, nh);
        }
    }

    RawMeta meta;
    meta.width = linear->width();
    meta.height = linear->height();
    meta.nativeWidth = nativeW;
    meta.nativeHeight = nativeH;
    // As-shot WB and camToProPhoto are RAW-only concepts; for an sRGB file the
    // buffer already sits in ProPhoto, so the identity defaults apply.
    return DecodedImage{std::move(linear), std::move(meta)};
}

} // namespace lrc
