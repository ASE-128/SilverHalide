#include "engine/ColorManager.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <lcms2.h>

#include "engine/ImageBuffer.h"
#include "utils/Parallel.h"

namespace lrc {

namespace {

// Bradford chromatic adaptation: XYZ D65 -> XYZ D50.
constexpr double kBradfordD65ToD50[3][3] = {
    {1.0478112, 0.0228866, -0.0501270},
    {0.0295424, 0.9904844, -0.0170491},
    {-0.0092345, 0.0150436, 0.7521316},
};

// XYZ (D50) -> linear ProPhoto RGB (Lindbloom).
constexpr double kXyzD50ToProPhoto[3][3] = {
    {1.3459433, -0.2556075, -0.0511118},
    {-0.5445989, 1.5081673, 0.0205351},
    {0.0000000, 0.0000000, 1.2118128},
};

// Pooled scratch for toDisplaySRGB's per-chunk lcms output. thread_local
// gives one buffer per worker thread (lock-free); it grows to the largest
// chunk size once and is reused thereafter. (pooled scratch)
thread_local std::vector<float> tlsScratch;

} // namespace

ColorManager::ColorManager() {
    srgb_ = cmsCreate_sRGBProfile();

    // Build a ProPhoto (ROMM RGB) profile from primaries + LINEAR TRC.
    // The pipeline works in linear light (gamma 1.0), so the working-space
    // profile must use a linear tone curve; otherwise the transform would
    // de-gamma the already-linear data.
    const cmsCIExyY d50 = *cmsD50_xyY();
    cmsCIExyYTRIPLE prim{};
    prim.Red.x = 0.7347;
    prim.Red.y = 0.2653;
    prim.Green.x = 0.1596;
    prim.Green.y = 0.8404;
    prim.Blue.x = 0.0366;
    prim.Blue.y = 0.0001;
    prim.Red.Y = prim.Green.Y = prim.Blue.Y = 1.0;
    cmsToneCurve* gamma = cmsBuildGamma(0, 1.0);
    cmsToneCurve* gammaRGB[3] = {gamma, gamma, gamma};
    prophoto_ = cmsCreateRGBProfile(&d50, &prim, gammaRGB);

    // libraw's output_color=4 workspace: the same ProPhoto primaries but
    // anchored at the D65 white point (dcraw normalizes the working-space white
    // to the camera's as-shot neutral). fromDcrawProPhoto re-anchors it to the
    // D50 ProPhoto working space via lcms.
    cmsCIExyY d65xyY{};
    cmsWhitePointFromTemp(&d65xyY, 6504.0);
    dcrawProPhoto_ = cmsCreateRGBProfile(&d65xyY, &prim, gammaRGB);
    cmsFreeToneCurve(gamma);

    display_ = srgb_;
    rebuildTransforms();
}

ColorManager::~ColorManager() {
    if (srgbToLinear_)
        cmsDeleteTransform(srgbToLinear_);
    if (linearToDisplay_)
        cmsDeleteTransform(linearToDisplay_);
    if (dcrawToProPhoto_)
        cmsDeleteTransform(dcrawToProPhoto_);
    if (prophoto_)
        cmsCloseProfile(prophoto_);
    if (dcrawProPhoto_)
        cmsCloseProfile(dcrawProPhoto_);
    // srgb_ may be shared with display_ when no display profile is loaded.
    if (display_ && display_ != srgb_)
        cmsCloseProfile(display_);
    if (srgb_)
        cmsCloseProfile(srgb_);
}

void ColorManager::rebuildTransforms() {
    if (linearToDisplay_)
        cmsDeleteTransform(linearToDisplay_);
    linearToDisplay_ = cmsCreateTransform(
        prophoto_, TYPE_RGB_FLT, display_, TYPE_RGB_FLT, INTENT_PERCEPTUAL,
        cmsFLAGS_HIGHRESPRECALC | cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE);
    if (srgbToLinear_)
        cmsDeleteTransform(srgbToLinear_);
    srgbToLinear_ = cmsCreateTransform(
        srgb_, TYPE_RGB_8, prophoto_, TYPE_RGB_FLT, INTENT_PERCEPTUAL,
        cmsFLAGS_HIGHRESPRECALC | cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE);
    // dcraw ProPhoto (D65) -> working ProPhoto (D50): independent of the display
    // profile, so it is (re)built alongside the others for symmetry.
    if (dcrawToProPhoto_)
        cmsDeleteTransform(dcrawToProPhoto_);
    dcrawToProPhoto_ = cmsCreateTransform(
        dcrawProPhoto_, TYPE_RGB_FLT, prophoto_, TYPE_RGB_FLT, INTENT_PERCEPTUAL,
        cmsFLAGS_HIGHRESPRECALC | cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE);
}

void ColorManager::loadDisplayProfile(const std::filesystem::path& iccPath) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    cmsHPROFILE candidate = cmsOpenProfileFromFile(iccPath.string().c_str(), "r");
    if (!candidate)
        return; // keep previous display profile
    if (display_ && display_ != srgb_)
        cmsCloseProfile(display_);
    display_ = candidate;
    rebuildTransforms();
}

std::vector<uint8_t> ColorManager::exportProfileBytes(
    ExportColorProfile profile) {
    cmsHPROFILE h = nullptr;
    switch (profile) {
        case ExportColorProfile::SRGB:
            h = cmsCreate_sRGBProfile();
            break;
        case ExportColorProfile::AdobeRGB: {
            // Adobe RGB (1998): primaries + D65 white point + 2.2-ish TRC.
            cmsCIExyY d65 = *cmsD50_xyY();
            cmsWhitePointFromTemp(&d65, 6504);
            const cmsCIExyYTRIPLE aPrimaries = {
                {0.6400, 0.3300, 1.0},  // red
                {0.2100, 0.7100, 1.0},  // green
                {0.1500, 0.0600, 1.0},  // blue
            };
            cmsToneCurve* gamma = cmsBuildGamma(nullptr, 2.2);
            cmsToneCurve* trc[3] = {gamma, gamma, gamma};
            h = cmsCreateRGBProfile(&d65, &aPrimaries, trc);
            cmsFreeToneCurve(gamma);
            break;
        }
        case ExportColorProfile::DisplayP3: {
            // Display P3 primaries + D65 white point + gamma 2.2 TRC (a common
            // SDR approximation; the ICC bytes just need to round-trip as the
            // selected profile).
            cmsCIExyY d65 = *cmsD50_xyY();
            cmsWhitePointFromTemp(&d65, 6504);
            const cmsCIExyYTRIPLE p3Primaries = {
                {0.6800, 0.3200, 1.0},  // red
                {0.2650, 0.6900, 1.0},  // green
                {0.1500, 0.0600, 1.0},  // blue
            };
            cmsToneCurve* gamma = cmsBuildGamma(nullptr, 2.2);
            cmsToneCurve* trc[3] = {gamma, gamma, gamma};
            h = cmsCreateRGBProfile(&d65, &p3Primaries, trc);
            cmsFreeToneCurve(gamma);
            break;
        }
    }
    if (!h)
        return {};

    cmsUInt32Number size = 0;
    if (!cmsSaveProfileToMem(h, nullptr, &size) || size == 0) {
        cmsCloseProfile(h);
        return {};
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!cmsSaveProfileToMem(h, bytes.data(), &size)) {
        cmsCloseProfile(h);
        return {};
    }
    cmsCloseProfile(h);
    return bytes;
}

ImageBufferPtr ColorManager::sRGBEncodedToProfile(
    const ImageBuffer& enc, const std::vector<uint8_t>& iccBytes) const {
    // Fallback used when no transform can run: hand back a copy of the input so
    // the caller never pairs wrong pixels against a missing/broken ICC label.
    auto clone = [&enc]() {
        auto c =
            ImageBuffer::create(enc.width(), enc.height(), enc.format());
        c->data() = enc.data();
        return c;
    };

    // sRGB source profile always exists (built in the constructor).
    if (iccBytes.empty() || !srgb_)
        return clone();

    cmsHPROFILE dst = cmsOpenProfileFromMem(
        iccBytes.data(), static_cast<cmsUInt32Number>(iccBytes.size()));
    if (!dst)
        return clone();

    const bool is16 = (enc.format() == PixelFormat::U16);
    const cmsUInt32Number fmt = is16 ? TYPE_RGB_16 : TYPE_RGB_8;
    cmsHTRANSFORM t = cmsCreateTransform(
        srgb_, fmt, dst, fmt, INTENT_RELATIVE_COLORIMETRIC,
        cmsFLAGS_HIGHRESPRECALC | cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE);
    cmsCloseProfile(dst);
    if (!t)
        return clone();

    auto out = ImageBuffer::create(enc.width(), enc.height(), enc.format());
    const cmsUInt32Number pixels = static_cast<cmsUInt32Number>(
        static_cast<size_t>(enc.width()) * static_cast<size_t>(enc.height()));
    cmsDoTransform(t, enc.data().data(), out->data().data(), pixels);
    cmsDeleteTransform(t);
    return out;
}

void ColorManager::linearProPhotoToSRGB(const float* src, float* dst,
                                        size_t pixelCount) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    cmsDoTransform(linearToDisplay_, src, dst, pixelCount);
    const size_t n = pixelCount * 3;
    for (size_t i = 0; i < n; ++i)
        dst[i] = std::clamp(dst[i], 0.0f, 1.0f);
}

ImageBufferPtr ColorManager::srgbToLinearProPhoto(
    const ImageBuffer& srgbU8) const {
    if (srgbU8.empty() || srgbU8.format() != PixelFormat::U8)
        return ImageBuffer::create(0, 0, PixelFormat::F32);
    const size_t pixelCount =
        static_cast<size_t>(srgbU8.width()) * srgbU8.height();
    auto out = ImageBuffer::create(srgbU8.width(), srgbU8.height(),
                                   PixelFormat::F32);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    cmsDoTransform(srgbToLinear_, srgbU8.data().data(), out->data().data(),
                   pixelCount);
    return out;
}

ImageBufferPtr ColorManager::sourceToLinearProPhoto(
    const ImageBuffer& src, const std::vector<uint8_t>& iccBytes) const {
    if (src.empty() ||
        (src.format() != PixelFormat::U8 && src.format() != PixelFormat::U16))
        return ImageBuffer::create(0, 0, PixelFormat::F32);

    // Source colour space: the embedded ICC when present, else a documented
    // sRGB fallback so ICC-less files behave exactly as before.
    cmsHPROFILE source = nullptr;
    if (!iccBytes.empty())
        source = cmsOpenProfileFromMem(iccBytes.data(),
                                       static_cast<cmsUInt32Number>(
                                           iccBytes.size()));
    if (!source)
        source = cmsCreate_sRGBProfile();

    const cmsUInt32Number inType =
        (src.format() == PixelFormat::U16) ? TYPE_RGB_16 : TYPE_RGB_8;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    // prophoto_ is immutable after construction; a per-call transform is cheap
    // for import-sized images (profiles are small, recreate per invocation and
    // cache nothing).
    cmsHTRANSFORM tr = cmsCreateTransform(
        source, inType, prophoto_, TYPE_RGB_FLT, INTENT_PERCEPTUAL,
        cmsFLAGS_HIGHRESPRECALC | cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE);

    ImageBufferPtr out;
    if (tr) {
        out = ImageBuffer::create(src.width(), src.height(), PixelFormat::F32);
        const size_t pixelCount =
            static_cast<size_t>(src.width()) * src.height();
        // 16-bit input is converted directly (TYPE_RGB_16), never truncated to
        // 8 bits first, so the extra precision survives the colour transform.
        cmsDoTransform(tr, src.data().data(), out->data().data(),
                       static_cast<cmsUInt32Number>(pixelCount));
        cmsDeleteTransform(tr);
    }
    cmsCloseProfile(source);  // sRGB fallback and mem-opened profiles both free here.

    if (!out)
        return ImageBuffer::create(0, 0, PixelFormat::F32);
    return out;
}

// Converts libraw's output_color=4 linear ProPhoto (D65 white) U16 buffer into
// the working linear ProPhoto (D50) U16 buffer. The pipeline treats every
// working buffer as D50 ProPhoto, so this is the single gate where libraw's
// dcraw white-point convention is re-anchored.
ImageBufferPtr ColorManager::fromDcrawProPhoto(
    const ImageBuffer& dcrawProPhotoLinear) const {
    if (dcrawProPhotoLinear.empty() ||
        dcrawProPhotoLinear.format() != PixelFormat::U16)
        return ImageBuffer::create(0, 0, PixelFormat::U16);
    const size_t pixelCount =
        static_cast<size_t>(dcrawProPhotoLinear.width()) *
        dcrawProPhotoLinear.height();
    // U16 -> F32 (normalized), then the lcms white-point adaptation, then back
    // to U16 so the pipeline sees the same storage as before.
    auto f32 = dcrawProPhotoLinear.toF32();
    auto out = ImageBuffer::create(dcrawProPhotoLinear.width(),
                                   dcrawProPhotoLinear.height(), PixelFormat::U16);
    uint16_t* dst = reinterpret_cast<uint16_t*>(out->data().data());
    const float* src = reinterpret_cast<const float*>(f32->data().data());
    const size_t grain = lrc::parallel::defaultGrain(pixelCount);
    lrc::parallel::parallelFor(pixelCount, grain, [&](size_t begin, size_t end) {
        const size_t count = end - begin;
        std::vector<float> tmp(count * 3);
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            cmsDoTransform(dcrawToProPhoto_, src + begin * 3, tmp.data(),
                           static_cast<cmsUInt32Number>(count));
        }
        for (size_t i = 0; i < count; ++i) {
            const float* s = tmp.data() + i * 3;
            uint16_t* d = dst + (begin + i) * 3;
            d[0] = static_cast<uint16_t>(std::clamp(s[0], 0.0f, 1.0f) * 65535.0f);
            d[1] = static_cast<uint16_t>(std::clamp(s[1], 0.0f, 1.0f) * 65535.0f);
            d[2] = static_cast<uint16_t>(std::clamp(s[2], 0.0f, 1.0f) * 65535.0f);
        }
    });
    return out;
}

// Single-pixel variant used by the point-colour sampler: maps the 8-bit sRGB
// pixel under the cursor back into linear ProPhoto reference RGB.
void ColorManager::srgbPixelToLinearProPhoto(uint8_t r, uint8_t g, uint8_t b,
                                             float& lr, float& lg,
                                             float& lb) const {
    auto src = ImageBuffer::create(1, 1, PixelFormat::U8);
    uint8_t* srcData = src->data().data();
    srcData[0] = r;
    srcData[1] = g;
    srcData[2] = b;

    auto dst = ImageBuffer::create(1, 1, PixelFormat::F32);

    std::shared_lock<std::shared_mutex> lock(mutex_);
    cmsDoTransform(srgbToLinear_, src->data().data(), dst->data().data(), 1);

    const float* out = reinterpret_cast<const float*>(dst->data().data());
    lr = out[0];
    lg = out[1];
    lb = out[2];
}

ImageBufferPtr ColorManager::toDisplaySRGB(
    const ImageBuffer& linearProPhoto) const {
    if (linearProPhoto.empty())
        return ImageBuffer::create(0, 0, PixelFormat::U8);
    const size_t pixelCount =
        static_cast<size_t>(linearProPhoto.width()) * linearProPhoto.height();

    // cmsDoTransform only reads the source, so when the input is already F32 we
    // can point straight at the caller's buffer and skip the full-buffer copy.
    // U16 input still goes through toF32() (owned here for its lifetime).
    ImageBufferPtr f32;
    const float* src;
    if (linearProPhoto.format() == PixelFormat::F32) {
        src = reinterpret_cast<const float*>(linearProPhoto.data().data());
    } else {
        f32 = linearProPhoto.toF32();
        src = reinterpret_cast<const float*>(f32->data().data());
    }

    auto out = ImageBuffer::create(linearProPhoto.width(),
                                   linearProPhoto.height(), PixelFormat::U8);
    uint8_t* dst = out->data().data();

    // Split the transform + quantize step over the thread pool. Each chunk
    // runs cmsDoTransform under the mutex (the transforms were built with
    // cmsFLAGS_NOCACHE so the profile data is read-only) and quantizes to U8
    // outside the lock, so the per-byte conversion runs in parallel.
    const size_t grain = lrc::parallel::defaultGrain(pixelCount);
    lrc::parallel::parallelFor(pixelCount, grain, [&](size_t begin, size_t end) {
        const size_t count = end - begin;
        tlsScratch.resize(count * 3);
        {
            // Shared lock: the transform is read-only (built with NOCACHE), so
            // chunks convert in parallel. Chunk sizes stay well below the
            // cmsUInt32Number pixel limit.
            std::shared_lock<std::shared_mutex> lock(mutex_);
            cmsDoTransform(linearToDisplay_, src + begin * 3, tlsScratch.data(),
                           static_cast<cmsUInt32Number>(count));
        }
        for (size_t i = 0; i < count; ++i) {
            const float* s = tlsScratch.data() + i * 3;
            uint8_t* d = dst + (begin + i) * 3;
            d[0] = static_cast<uint8_t>(std::clamp(s[0], 0.0f, 1.0f) * 255.0f);
            d[1] = static_cast<uint8_t>(std::clamp(s[1], 0.0f, 1.0f) * 255.0f);
            d[2] = static_cast<uint8_t>(std::clamp(s[2], 0.0f, 1.0f) * 255.0f);
        }
    });
    return out;
}

std::array<double, 9> ColorManager::xyzD65ToProPhoto() {
    // M = XyzD50ToProPhoto * Bradford(D65->D50)
    std::array<double, 9> m{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k)
                sum += kXyzD50ToProPhoto[row][k] * kBradfordD65ToD50[k][col];
            m[static_cast<size_t>(row) * 3 + col] = sum;
        }
    }
    return m;
}

std::array<double, 9> ColorManager::cameraToProPhoto(
    const std::array<double, 9>& camToXyzD65) {
    const std::array<double, 9> xyzToPro =
        xyzD65ToProPhoto(); // XYZ(D65) -> ProPhoto
    std::array<double, 9> m{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k)
                sum += xyzToPro[static_cast<size_t>(row) * 3 + k] *
                       camToXyzD65[static_cast<size_t>(k) * 3 + col];
            m[static_cast<size_t>(row) * 3 + col] = sum;
        }
    }
    return m;
}

} // namespace lrc
