#include "engine/RawDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <libraw/libraw.h>

#include "engine/ColorManager.h"
#include "engine/DcpProfile.h"
#include "engine/ImageFormats.h"
#include "engine/ImageIO.h"
#include "utils/MathUtils.h"
#include "utils/Parallel.h"

namespace lrc {

namespace {

bool rawOk(const char* what, int ret) {
    return ret == LIBRAW_SUCCESS;
}

// Assembles the camera / lens / exposure metadata and native dimensions from an
// opened LibRaw instance. Shared by decode() and readMeta() so both report
// identical EXIF-derived values. `width`/`height` are the "as-decoded" frame
// dimensions (halved for half-size previews); nativeWidth/Height are always the
// full-resolution, rotation-corrected dimensions.
RawMeta makeRawMeta(const LibRaw& raw, int width, int height) {
    RawMeta meta;
    meta.make = std::string(raw.imgdata.idata.make);
    meta.model = std::string(raw.imgdata.idata.model);
    meta.lensMaker = std::string(raw.imgdata.lens.LensMake);
    meta.lensModel = std::string(raw.imgdata.lens.Lens);
    meta.focalLengthMm = raw.imgdata.other.focal_len;
    meta.aperture = raw.imgdata.other.aperture;
    meta.cropFactor = raw.imgdata.lens.FocalLengthIn35mmFormat > 0.0f &&
                              meta.focalLengthMm > 0.0f
                          ? raw.imgdata.lens.FocalLengthIn35mmFormat /
                                meta.focalLengthMm
                          : 1.0f;
    meta.width = width;
    meta.height = height;
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
    return meta;
}

// Inverts a non-singular 3x3 matrix (row-major) via the adjugate/determinant
// formula. Used to compute the Moore–Penrose pseudo-inverse below.
bool invert3x3(const double m[3][3], double out[3][3]) {
    const double a = m[0][0], b = m[0][1], c = m[0][2];
    const double d = m[1][0], e = m[1][1], f = m[1][2];
    const double g = m[2][0], h = m[2][1], i = m[2][2];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) +
                       c * (d * h - e * g);
    if (det == 0.0)
        return false;
    const double inv = 1.0 / det;
    out[0][0] = (e * i - f * h) * inv;
    out[0][1] = (c * h - b * i) * inv;
    out[0][2] = (b * f - c * e) * inv;
    out[1][0] = (f * g - d * i) * inv;
    out[1][1] = (a * i - c * g) * inv;
    out[1][2] = (c * d - a * f) * inv;
    out[2][0] = (d * h - e * g) * inv;
    out[2][1] = (b * g - a * h) * inv;
    out[2][2] = (a * e - b * d) * inv;
    return true;
}

// Approximate correlated color temperature (Kelvin) of a DNG LightSource
// code (tag 50778/50779). Unknown codes fall back to D65; only the codes
// Adobe actually ships in camera profiles are mapped.
double lightSourceCct(float lightSource) {
    const int code = static_cast<int>(lightSource);
    switch (code) {
    case 17: return 2856.0;  // Standard illuminant A
    case 18: return 4874.0;  // Standard illuminant B
    case 19: return 6774.0;  // Standard illuminant C
    case 20: return 5503.0;  // D55
    case 21: return 6504.0;  // D65
    case 22: return 7504.0;  // D75
    case 23: return 5003.0;  // D50
    case 24: return 3200.0;  // ISO studio tungsten
    default: return 6504.0;
    }
}

// Correlated color temperature of the scene white implied by a set of white
// balance multipliers, via the DNG dual-illuminant model: the neutral camera
// response (inverse gains, normalized) is pushed through the inverse camera
// matrix into XYZ D50, reduced to chromaticity, and converted with McCamy's
// cubic approximation. Returns 0 when the matrix is singular.
double whiteBalanceCct(const std::array<double, 3>& camMul,
                       const std::array<float, 9>& camToXyz) {
    double m[3][3] = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m[i][j] = camToXyz[i * 3 + j];
    double inv[3][3];
    if (!invert3x3(m, inv))
        return 0.0;
    // Camera neutral: the channel responses that render as D50 white are the
    // inverse of the WB gains. Normalize to sum 1 like the DNG spec's
    // CameraNeutral before mapping into XYZ.
    double neutral[3] = {1.0 / std::max(camMul[0], 1e-6),
                         1.0 / std::max(camMul[1], 1e-6),
                         1.0 / std::max(camMul[2], 1e-6)};
    const double sum = neutral[0] + neutral[1] + neutral[2];
    if (sum <= 0.0)
        return 0.0;
    double xyz[3] = {};
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k)
            xyz[i] += inv[i][k] * neutral[k] / sum;
    const double total = xyz[0] + xyz[1] + xyz[2];
    if (total <= 0.0)
        return 0.0;
    const double x = xyz[0] / total;
    const double y = xyz[1] / total;
    const double n = (x - 0.3320) / (0.1858 - y);
    const double cct = 449.0 * n * n * n + 3525.0 * n * n + 6823.3 * n + 5520.33;
    // McCamy drifts outside a sane range for extreme inputs; clamp so the
    // interpolation weight stays well-defined.
    return std::clamp(cct, 1500.0, 15000.0);
}

// Interpolation fraction of calibration state 2 (0 = state 1, 1 = state 2)
// for a white balance CCT, per the DNG spec's inverse-distance rule.
double illuminant2Weight(double cct, double cct1, double cct2) {
    if (cct <= 0.0 || cct1 <= 0.0 || cct2 <= 0.0 || std::abs(cct1 - cct2) < 1.0)
        return 0.0;
    const double w =
        (1.0 / cct1 - 1.0 / cct) / (1.0 / cct1 - 1.0 / cct2);
    return std::clamp(w, 0.0, 1.0);
}

} // namespace

std::array<float, 9> RawDecoder::dcpColorMatrix(
    const DcpProfile& dcp, const std::array<double, 3>& camMul) {
    std::array<float, 9> matrix = dcp.colorMatrix1;
    if (!dcp.hasColorMatrix2 || !dcp.hasCalibrationIlluminant1 ||
        !dcp.hasCalibrationIlluminant2)
        return matrix;
    const double cct =
        whiteBalanceCct(camMul, dcp.colorMatrix1);
    const double w =
        illuminant2Weight(cct, lightSourceCct(dcp.calibrationIlluminant[0]),
                          lightSourceCct(dcp.calibrationIlluminant[1]));
    for (size_t i = 0; i < matrix.size(); ++i)
        matrix[i] = dcp.colorMatrix1[i] *
                        static_cast<float>(1.0 - w) +
                    dcp.colorMatrix2[i] * static_cast<float>(w);
    return matrix;
}

namespace {

// Re-target libraw's colour pipeline at a DCP camera->XYZ matrix instead of
// its built-in camera matrix.
//
// libraw applies its camera matrix through the public `imgdata.color.rgb_cam`
// member, which `dcraw_process()` -> `convert_to_rgb()` reads verbatim. We
// recompute rgb_cam from the DCP matrix, replicating libraw's own derivation
// (`cam_xyz_coeff`: camera->XYZ matrix composed with the sRGB XYZ basis,
// row-normalised, then pseudo-inverted and transposed into rgb_cam). Because
// `cam_xyz_coeff` is protected, we reproduce the arithmetic here. The matrix
// follows the DNG dual-illuminant model: when the profile carries two
// calibration states plus their illuminants, they are interpolated by the
// correlated color temperature of the as-shot white balance (see
// dcpColorMatrix).
//
// IMPORTANT: rgb_cam and the as-shot white balance (`pre_mul`) are only weakly
// coupled; we write rgb_cam and NEVER touch pre_mul, so the sensor's WB gains
// stay exactly as libraw calibrated them. Re-deriving white balance from an
// arbitrary DCP matrix is the failure this decoder already documented ("every
// RAW came out green") — we avoid it by construction.
//
// `dcp` stays owned by the caller; this helper only reads it.
void injectDcpColorMatrix(LibRaw& raw, const DcpProfile& dcp,
                          const std::array<double, 3>& camMul) {
    if (!dcp.hasColorMatrix1)
        return;
    const std::array<float, 9> dcpMatrix = RawDecoder::dcpColorMatrix(dcp, camMul);
    // Location of xyz_rgb in libraw (XYZ_from_sRGB, D65) — mirroring the
    // constant the library uses in cam_xyz_coeff.
    static const double kXyzRgb[3][3] = {
        {0.4124564, 0.3575761, 0.1804375},
        {0.2126729, 0.7151522, 0.0721750},
        {0.0193339, 0.1191920, 0.9503041}};
    const int colors = raw.imgdata.idata.colors;
    if (colors < 3)
        return;

    // DCP ColorMatrix1 maps camera RGB -> XYZ(D50) with rows = X,Y,Z and
    // columns = R,G,B. libraw's cam_xyz is [colorChannel][XYZ], so transpose
    // the DCP matrix onto that channel-major layout, then compose with the RGB
    // basis exactly as cam_xyz_coeff does.
    double camRgb[4][3] = {};
    for (int c = 0; c < colors; ++c)
        for (int j = 0; j < 3; ++j) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) {
                const double camXyz =
                    c < 3 ? dcpMatrix[k * 3 + c] : 0.0;
                sum += camXyz * kXyzRgb[k][j];
            }
            camRgb[c][j] = sum;
        }
    // Row-normalise (dcraw normalises each camera channel so its sum is 1).
    for (int c = 0; c < colors; ++c) {
        double rowSum = camRgb[c][0] + camRgb[c][1] + camRgb[c][2];
        if (rowSum > 1e-5)
            for (int j = 0; j < 3; ++j)
                camRgb[c][j] /= rowSum;
    }

    // Moore–Penrose pseudo-inverse rgb_cam = (A^T A)^{-1} A^T: A is colors x 3
    // (camRgb), so A^T A is 3x3 (invertible for 3>=3 independent channels).
    double ata[3][3] = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < colors; ++k)
                ata[i][j] += camRgb[k][i] * camRgb[k][j];
    double ataInv[3][3];
    if (!invert3x3(ata, ataInv))
        return;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < colors; ++j) {
            double v = 0.0;
            for (int k = 0; k < 3; ++k)
                v += ataInv[i][k] * camRgb[j][k];
            // dcraw stores the transpose: rgb_cam[i][j] = pseudoInv[j][i].
            raw.imgdata.color.rgb_cam[i][j] = static_cast<float>(v);
        }
}

// sRGB transfer and its inverse (IEC 61966-2-1), used for the encoded value
// index of DCP hue/sat maps with ProfileHueSatMapEncoding = 1.
float linearToSrgb(float v) {
    return v <= 0.0031308f ? v * 12.92f
                           : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

float srgbToLinear(float v) {
    return v <= 0.04045f ? v / 12.92f
                         : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

// Hue of an RGB triplet in turns (0..1, wraps); 0 = red, 1/3 = green,
// 2/3 = blue. Matches the HSV definition the DNG hue/sat map indexes with.
float rgbHue01(float r, float g, float b, float maxc, float minc) {
    const float delta = maxc - minc;
    if (delta <= 1e-8f)
        return 0.0f;
    float h;
    if (maxc == r)
        h = (g - b) / delta;
    else if (maxc == g)
        h = (b - r) / delta + 2.0f;
    else
        h = (r - g) / delta + 4.0f;
    h /= 6.0f;
    if (h < 0.0f)
        h += 1.0f;
    return h;
}

void hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    if (s <= 0.0f) {
        r = g = b = v;
        return;
    }
    h = h - std::floor(h);
    const float f = h * 6.0f;
    const int i = static_cast<int>(f);
    const float frac = f - static_cast<float>(i);
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * frac);
    const float t = v * (1.0f - s * (1.0f - frac));
    switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
}

} // namespace

ImageBufferPtr RawDecoder::applyDcpHueSatMap(
    const ImageBufferPtr& src, const DcpProfile& dcp,
    const std::array<double, 3>& camMul) {
    if (!src || src->format() != PixelFormat::F32 || !dcp.hasHueSatMap1)
        return src;
    const uint32_t hd = dcp.hueSatDims[0];
    const uint32_t sd = dcp.hueSatDims[1];
    const uint32_t vd = dcp.hueSatDims[2];
    if (hd == 0 || sd < 2 || vd == 0)
        return src;

    // Dual-illuminant profiles interpolate the two maps by the same white
    // balance weight as the colour matrix. The blend happens once per decode,
    // not per pixel.
    std::vector<float> blended;
    const float* map = dcp.hueSatMap1.data();
    if (dcp.hasHueSatMap2 && dcp.hueSatMap2.size() == dcp.hueSatMap1.size() &&
        dcp.hasCalibrationIlluminant1 && dcp.hasCalibrationIlluminant2 &&
        dcp.hasColorMatrix1) {
        const double cct = whiteBalanceCct(camMul, dcp.colorMatrix1);
        const double w =
            illuminant2Weight(cct, lightSourceCct(dcp.calibrationIlluminant[0]),
                              lightSourceCct(dcp.calibrationIlluminant[1]));
        if (w > 0.0) {
            blended.resize(dcp.hueSatMap1.size());
            const float f = static_cast<float>(w);
            for (size_t i = 0; i < blended.size(); ++i)
                blended[i] = dcp.hueSatMap1[i] * (1.0f - f) +
                             dcp.hueSatMap2[i] * f;
            map = blended.data();
        }
    }

    // Per DNG 1.4 the value index is sRGB-encoded only when the profile says
    // so (encoding 1) AND the table is truly 3D (encoding is not applicable
    // to 2.5D tables with a single value division).
    const bool encodeValue = dcp.hueSatMapEncoding == 1 && vd > 1;

    // Grid strides: entries are stored value-outer, hue-middle,
    // saturation-inner, three floats (hue shift degrees, saturation scale,
    // value scale) per cell. Advancing one hue step skips sd cells; advancing
    // one value step skips hd*sd cells.
    const size_t hueStride = static_cast<size_t>(sd) * 3;
    const size_t valStride = static_cast<size_t>(hd) * sd * 3;
    const auto sample = [&](uint32_t h, uint32_t s, uint32_t v) {
        return &map[static_cast<size_t>(v) * valStride +
                    static_cast<size_t>(h) * hueStride + s * 3];
    };

    const float* in = reinterpret_cast<const float*>(src->data().data());
    const size_t nPixels = static_cast<size_t>(src->width()) * src->height();
    auto out = ImageBuffer::create(src->width(), src->height(),
                                   PixelFormat::F32);
    float* o = reinterpret_cast<float*>(out->data().data());

    lrc::parallel::parallelFor(
        nPixels, lrc::parallel::defaultGrain(nPixels),
        [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const size_t base = i * 3;
                float r = in[base];
                float g = in[base + 1];
                float b = in[base + 2];
                const float maxc = std::max({r, g, b});
                if (maxc <= 1e-8f) {
                    // Black carries no hue; the map cannot move it.
                    o[base] = r;
                    o[base + 1] = g;
                    o[base + 2] = b;
                    continue;
                }
                const float minc = std::min({r, g, b});
                const float v0 = maxc;
                const float s0 = (v0 - minc) / v0;
                const float h0 = rgbHue01(r, g, b, maxc, minc);

                // Grid coordinates: hue wraps (position on a ring), saturation
                // and value clamp at the table edges.
                float vp = v0;
                if (encodeValue)
                    vp = linearToSrgb(v0);
                const float hp = h0 * static_cast<float>(hd);
                const uint32_t h0i =
                    static_cast<uint32_t>(hp) % hd;
                const uint32_t h1i = (h0i + 1) % hd;
                const float fh = hp - std::floor(hp);
                const float sp = std::clamp(s0, 0.0f, 1.0f) *
                                 static_cast<float>(sd - 1);
                const uint32_t s0i = static_cast<uint32_t>(sp);
                const uint32_t s1i = std::min(s0i + 1, sd - 1);
                const float fs = sp - static_cast<float>(s0i);
                const float vpg = std::clamp(vp, 0.0f, 1.0f) *
                                  static_cast<float>(vd - 1);
                const uint32_t v0i = static_cast<uint32_t>(vpg);
                const uint32_t v1i = std::min(v0i + 1, vd - 1);
                const float fv = vpg - static_cast<float>(v0i);

                // Trilinear interpolation of (dHue, dSat, dVal).
                float dh = 0.0f, ds = 0.0f, dv = 0.0f;
                for (int c = 0; c < 3; ++c) {
                    const float c00 =
                        sample(h0i, s0i, v0i)[c] * (1.0f - fh) +
                        sample(h1i, s0i, v0i)[c] * fh;
                    const float c01 =
                        sample(h0i, s1i, v0i)[c] * (1.0f - fh) +
                        sample(h1i, s1i, v0i)[c] * fh;
                    const float c10 =
                        sample(h0i, s0i, v1i)[c] * (1.0f - fh) +
                        sample(h1i, s0i, v1i)[c] * fh;
                    const float c11 =
                        sample(h0i, s1i, v1i)[c] * (1.0f - fh) +
                        sample(h1i, s1i, v1i)[c] * fh;
                    const float c0 = c00 * (1.0f - fs) + c01 * fs;
                    const float c1 = c10 * (1.0f - fs) + c11 * fs;
                    const float d = c0 * (1.0f - fv) + c1 * fv;
                    if (c == 0) dh = d;
                    else if (c == 1) ds = d;
                    else dv = d;
                }

                // Apply: hue shift additive (degrees, wrapping), saturation
                // and value multiplicative scale factors.
                float h = h0 + dh / 360.0f;
                float s = std::clamp(s0 * ds, 0.0f, 1.0f);
                float v = vp * dv;
                if (encodeValue)
                    v = srgbToLinear(v);
                v = std::clamp(v, 0.0f, 1.0f);
                hsvToRgb(h, s, v, o[base], o[base + 1], o[base + 2]);
            }
        });
    return out;
}

namespace {

// Applies a DCP tone curve (uniform input domain across its samples) to the
// working-space linear buffer. F32 working buffers are normalized to [0,1],
// so the curve domain is 1.0: input x maps uniformly onto the sample range.
// Out-of-range inputs clamp to the curve ends. Applied after the hue/sat
// map, matching the DNG pipeline order (matrix -> hue/sat map -> tone curve).
ImageBufferPtr applyDcpToneCurve(const ImageBufferPtr& src,
                                 const DcpProfile& dcp) {
    if (!src || src->format() != PixelFormat::F32 || dcp.toneCurveSize < 2)
        return src;
    const float* in =
        reinterpret_cast<const float*>(src->data().data());
    const size_t nPixels = static_cast<size_t>(src->width()) * src->height();
    auto out = ImageBuffer::create(src->width(), src->height(),
                                   PixelFormat::F32);
    float* o = reinterpret_cast<float*>(out->data().data());
    const float last = static_cast<float>(dcp.toneCurveSize - 1);
    lrc::parallel::parallelFor(
        nPixels, lrc::parallel::defaultGrain(nPixels),
        [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const size_t base = i * 3;
                for (int c = 0; c < 3; ++c) {
                    float v = in[base + c];
                    if (v <= 0.0f) {
                        o[base + c] = dcp.toneCurve[0];
                        continue;
                    }
                    if (v >= 1.0f) {
                        o[base + c] = dcp.toneCurve[dcp.toneCurveSize - 1];
                        continue;
                    }
                    const float xf = v * last;
                    const long i0 = static_cast<long>(xf);
                    const long i1 = i0 < static_cast<long>(last) ? i0 + 1 : i0;
                    const float frac = xf - static_cast<float>(i0);
                    o[base + c] =
                        dcp.toneCurve[i0] * (1.0f - frac) +
                        dcp.toneCurve[i1] * frac;
                }
            }
        });
    return out;
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

std::optional<RawMeta> RawDecoder::readMeta(
    const std::filesystem::path& path) const {
    // Metadata-only reads are only meaningful for RAW files; the JPEG/PNG/TIFF
    // path has no libraw instance to open (EXIF extraction for those formats is
    // out of scope for this lightweight API).
    if (!isRawPath(path))
        return std::nullopt;
    LibRaw raw;
    if (!rawOk("open", openRawFile(raw, path)))
        return std::nullopt;
    // No unpack / process: the reported frame dimensions are the raw's native
    // full-res sizes (this path never halves).
    return makeRawMeta(raw, raw.imgdata.sizes.width, raw.imgdata.sizes.height);
}

std::optional<DecodedImage> RawDecoder::decode(
    const std::filesystem::path& path, const DecodeParams& params,
    const DcpProfile* dcp) const {
    // Non-RAW formats (JPEG/PNG/TIFF) never reach libraw: they are already
    // demosaiced and rendered, so libraw would mis-treat them as camera data.
    if (!isRawPath(path))
        return decodeNonRaw(path, params);

    LibRaw raw;
    if (!rawOk("open", openRawFile(raw, path)))
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

    // Swap libraw's built-in camera matrix for a user-selected DCP colour
    // matrix. Must run after open_file (rgb_cam is computed in identify) and
    // before dcraw_process (whose convert_to_rgb() snapshots rgb_cam). The
    // white balance (pre_mul) is preserved by the helper.
    std::array<double, 3> camMul = {1.0, 1.0, 1.0};
    if (dcp) {
        const auto& mul = raw.imgdata.color.cam_mul;
        camMul = {static_cast<double>(mul[0]), static_cast<double>(mul[1]),
                  static_cast<double>(mul[2])};
        injectDcpColorMatrix(raw, *dcp, camMul);
    }

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

    RawMeta meta = makeRawMeta(raw, img->width, img->height);

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

    // Apply the DCP colour grade (hue/sat map, then tone curve) in the linear
    // working space, in DNG pipeline order after the colour matrix. The GUI
    // path leaves `linear` as U16 (fromDcrawProPhoto round-trips through
    // lcms and back), so upconvert to F32 for the grade and convert back to
    // honour the decoded contract (U16 for RAW). Zero-cost no-op when the
    // profile carries neither a map nor a curve.
    if (dcp && linear) {
        const bool roundTripU16 = linear->format() == PixelFormat::U16;
        if (roundTripU16)
            linear = linear->toF32();
        if (linear && linear->format() == PixelFormat::F32) {
            linear = applyDcpHueSatMap(linear, *dcp, camMul);
            linear = applyDcpToneCurve(linear, *dcp);
            if (roundTripU16 && linear)
                linear = linear->toU16();
        }
    }

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
    if (!rawOk("open", openRawFile(raw, path)))
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

    // Load the source at native bit depth and with its embedded ICC profile, so
    // a wide-gamut (Adobe RGB / Display P3) or 16-bit file is converted from its
    // true source space instead of being assumed sRGB at 8-bit.
    auto src = ImageIO::loadSourceImage(path);
    if (!src.pixels || src.pixels->empty())
        return std::nullopt;

    // Convert to the same linear ProPhoto working space the RAW path uses, so
    // the rest of the pipeline treats every import identically. An empty ICC
    // falls back to sRGB (the documented convention for untagged files).
    auto linear = cm_->sourceToLinearProPhoto(*src.pixels, src.icc);
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

    // FNV-1a over the embedded source ICC so a catalog preview can record which
    // colour space produced these pixels. 0 == untagged sRGB source.
    uint32_t cc = 2166136261u;
    for (uint8_t b : src.icc) {
        cc ^= b;
        cc *= 16777619u;
    }
    return DecodedImage{std::move(linear), std::move(meta),
                        src.icc.empty() ? 0u : cc};
}

} // namespace lrc
