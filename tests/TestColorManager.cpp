#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <lcms2.h>

#include "engine/ColorManager.h"
#include "engine/ImageBuffer.h"

using namespace Catch;
using namespace lrc;

TEST_CASE("ColorManager: D65 white maps to ProPhoto white", "[color]") {
    const std::array<double, 9> m = ColorManager::xyzD65ToProPhoto();
    // D65 white in XYZ.
    const double X = 0.95047, Y = 1.0, Z = 1.08883;
    const double r = m[0] * X + m[1] * Y + m[2] * Z;
    const double g = m[3] * X + m[4] * Y + m[5] * Z;
    const double b = m[6] * X + m[7] * Y + m[8] * Z;
    // After Bradford D65->D50 this is ProPhoto white (1,1,1).
    REQUIRE(r == Approx(1.0).margin(1e-3));
    REQUIRE(g == Approx(1.0).margin(1e-3));
    REQUIRE(b == Approx(1.0).margin(1e-3));
}

TEST_CASE("ColorManager: identity camera matrix yields the XYZ->ProPhoto map",
          "[color]") {
    const std::array<double, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
    const auto m = ColorManager::cameraToProPhoto(identity);
    const auto ref = ColorManager::xyzD65ToProPhoto();
    for (size_t i = 0; i < 9; ++i)
        REQUIRE(m[i] == Approx(ref[i]));
}

TEST_CASE("ColorManager: neutral gray renders as near-neutral sRGB",
          "[color]") {
    ColorManager cm;
    auto buf = ImageBuffer::create(4, 4, PixelFormat::F32);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            buf->setPixel(x, y, 0, 0.18f);
            buf->setPixel(x, y, 1, 0.18f);
            buf->setPixel(x, y, 2, 0.18f);
        }
    auto out = cm.toDisplaySRGB(*buf);
    REQUIRE(out->format() == PixelFormat::U8);
    REQUIRE(out->width() == 4);
    REQUIRE(out->height() == 4);

    const uint8_t* p = out->data().data();
    // 0.18 linear encodes to ~117 sRGB.
    REQUIRE(p[0] > 80);
    REQUIRE(p[0] < 160);
    REQUIRE(std::abs(static_cast<int>(p[0]) - p[1]) <= 8);
    REQUIRE(std::abs(static_cast<int>(p[1]) - p[2]) <= 8);
}

TEST_CASE("ColorManager: pure red has correct hue in sRGB", "[color]") {
    ColorManager cm;
    auto buf = ImageBuffer::create(1, 1, PixelFormat::F32);
    buf->setPixel(0, 0, 0, 0.5f);
    buf->setPixel(0, 0, 1, 0.0f);
    buf->setPixel(0, 0, 2, 0.0f);
    auto out = cm.toDisplaySRGB(*buf);
    const uint8_t* p = out->data().data();
    REQUIRE(p[0] > p[1]);
    REQUIRE(p[0] > p[2]);
}

TEST_CASE("ColorManager: toDisplaySRGB F32 input matches the float reference",
          "[color]") {
    ColorManager cm;
    const int w = 3, h = 2;
    auto buf = ImageBuffer::create(w, h, PixelFormat::F32);
    const float vals[6][3] = {
        {0.18f, 0.18f, 0.18f}, {0.5f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.9f},     {0.05f, 0.3f, 0.7f}, {1.0f, 1.0f, 1.0f},
    };
    float* d = reinterpret_cast<float*>(buf->data().data());
    for (int i = 0; i < w * h; ++i)
        for (int c = 0; c < 3; ++c)
            d[i * 3 + c] = vals[i][c];

    auto out = cm.toDisplaySRGB(*buf);
    REQUIRE(out->format() == PixelFormat::U8);
    REQUIRE(out->width() == w);
    REQUIRE(out->height() == h);

    // Reference: the same transform + clamp, without the U8 quantization step.
    std::vector<float> ref(static_cast<size_t>(w * h * 3));
    cm.linearProPhotoToSRGB(reinterpret_cast<const float*>(buf->data().data()),
                            ref.data(), static_cast<size_t>(w * h));
    const uint8_t* p = out->data().data();
    for (size_t i = 0; i < ref.size(); ++i)
        REQUIRE(p[i] ==
                static_cast<uint8_t>(std::clamp(ref[i], 0.0f, 1.0f) * 255.0f));
}

TEST_CASE("ColorManager: U16 input to toDisplaySRGB matches the F32 path",
          "[color]") {
    ColorManager cm;
    const int w = 64, h = 48;
    auto u16 = ImageBuffer::create(w, h, PixelFormat::U16);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            u16->setPixel(x, y, 0,
                          static_cast<float>(x) / static_cast<float>(w - 1));
            u16->setPixel(x, y, 1,
                          static_cast<float>(y) / static_cast<float>(h - 1));
            u16->setPixel(x, y, 2,
                          static_cast<float>((x + y) % 17) / 16.0f);
        }
    // Exact F32 representation of the U16 codes that toDisplaySRGB converts
    // internally for a U16 source.
    auto f32 = u16->toF32();

    auto outU16 = cm.toDisplaySRGB(*u16);
    auto outF32 = cm.toDisplaySRGB(*f32);
    REQUIRE(outU16->format() == PixelFormat::U8);
    REQUIRE(outU16->width() == w);
    REQUIRE(outU16->height() == h);
    REQUIRE(outU16->sizeBytes() == outF32->sizeBytes());
    REQUIRE(std::memcmp(outU16->data().data(), outF32->data().data(),
                        outU16->sizeBytes()) == 0);
}

// --- M17: loadDisplayProfile -----------------------------------------------

namespace {

// Builds an RGB ICC profile with a pure linear (gamma 1.0) tone curve and sRGB
// primaries, then writes it to `path`. Returns true on success. The bytes are
// written through std::ofstream (Unicode-safe) rather than cmsSaveProfileToFile,
// which uses the ANSI code page and would break on non-ANSI temp paths.
bool writeLinearRgbIcc(const std::filesystem::path& path) {
    const cmsCIExyY d50 = *cmsD50_xyY();
    const cmsCIExyYTRIPLE prim = {
        {0.6400f, 0.3300f, 1.0f},
        {0.3000f, 0.6000f, 1.0f},
        {0.1500f, 0.0600f, 1.0f},
    };
    cmsToneCurve* gamma = cmsBuildGamma(nullptr, 1.0);
    cmsToneCurve* curves[3] = {gamma, gamma, gamma};
    cmsHPROFILE prof = cmsCreateRGBProfile(&d50, &prim, curves);
    cmsFreeToneCurve(gamma);
    if (!prof)
        return false;

    cmsUInt32Number bytesNeeded = 0;
    if (!cmsSaveProfileToMem(prof, nullptr, &bytesNeeded) || bytesNeeded == 0) {
        cmsCloseProfile(prof);
        return false;
    }
    std::vector<cmsUInt8Number> data(bytesNeeded);
    if (!cmsSaveProfileToMem(prof, data.data(), &bytesNeeded)) {
        cmsCloseProfile(prof);
        return false;
    }
    cmsCloseProfile(prof);

    std::ofstream os(path, std::ios::binary);
    if (!os)
        return false;
    os.write(reinterpret_cast<const char*>(data.data()),
             static_cast<std::streamsize>(data.size()));
    return os.good();
}

// A single neutral mid-gray (0.5 linear ProPhoto) F32 buffer.
ImageBufferPtr makeMidGray() {
    auto buf = ImageBuffer::create(1, 1, PixelFormat::F32);
    buf->setPixel(0, 0, 0, 0.5f);
    buf->setPixel(0, 0, 1, 0.5f);
    buf->setPixel(0, 0, 2, 0.5f);
    return buf;
}

} // namespace

TEST_CASE("ColorManager: loadDisplayProfile applies the loaded profile",
          "[color][m17]") {
    const auto tmp = std::filesystem::temp_directory_path();
    const auto iccPath = tmp / "lrc_test_linear_display.icc";
    REQUIRE(writeLinearRgbIcc(iccPath));

    ColorManager cm;
    auto buf = makeMidGray();

    // Default display profile is sRGB: linear 0.5 -> ~187.
    const uint8_t srgbOut = cm.toDisplaySRGB(*buf)->data()[0];
    REQUIRE(srgbOut > 150);

    // A linear-gamma display profile maps linear 0.5 straight through (~127).
    cm.loadDisplayProfile(iccPath);
    const uint8_t linearOut = cm.toDisplaySRGB(*buf)->data()[0];
    REQUIRE(linearOut < 150);
    REQUIRE(linearOut != srgbOut);

    std::error_code ec;
    std::filesystem::remove(iccPath, ec);
}

TEST_CASE("ColorManager: loadDisplayProfile with a missing path keeps sRGB",
          "[color][m17]") {
    ColorManager cm;
    auto buf = makeMidGray();

    auto before = cm.toDisplaySRGB(*buf);
    // The directory does not exist: the open fails and the profile is kept.
    const auto missing =
        std::filesystem::temp_directory_path() / "lrc_no_such_dir_m17" /
        "missing.icc";
    REQUIRE_NOTHROW(cm.loadDisplayProfile(missing));
    auto after = cm.toDisplaySRGB(*buf);
    REQUIRE(after->sizeBytes() == before->sizeBytes());
    REQUIRE(std::memcmp(after->data().data(), before->data().data(),
                        before->sizeBytes()) == 0);
}

TEST_CASE("ColorManager: loadDisplayProfile with garbage bytes keeps previous",
          "[color][m17]") {
    const auto tmp = std::filesystem::temp_directory_path();
    const auto garbagePath = tmp / "lrc_test_garbage.icc";
    {
        std::ofstream os(garbagePath, std::ios::binary);
        os << "this is not an ICC profile at all";
    }

    ColorManager cm;
    auto buf = makeMidGray();
    auto before = cm.toDisplaySRGB(*buf);
    REQUIRE_NOTHROW(cm.loadDisplayProfile(garbagePath));
    auto after = cm.toDisplaySRGB(*buf);
    REQUIRE(std::memcmp(after->data().data(), before->data().data(),
                        before->sizeBytes()) == 0);

    std::error_code ec;
    std::filesystem::remove(garbagePath, ec);
}

TEST_CASE("ColorManager: failed loadDisplayProfile keeps a loaded custom profile",
          "[color][m17]") {
    const auto tmp = std::filesystem::temp_directory_path();
    const auto iccPath = tmp / "lrc_test_linear_display.icc";
    REQUIRE(writeLinearRgbIcc(iccPath));

    ColorManager cm;
    auto buf = makeMidGray();
    cm.loadDisplayProfile(iccPath);  // custom linear profile active
    const uint8_t customOut = cm.toDisplaySRGB(*buf)->data()[0];

    // A subsequent failed load must not reset to sRGB: the implementation keeps
    // the previously loaded display profile.
    const auto missing =
        std::filesystem::temp_directory_path() / "lrc_no_such_dir_m17" /
        "missing.icc";
    REQUIRE_NOTHROW(cm.loadDisplayProfile(missing));
    const uint8_t afterFail = cm.toDisplaySRGB(*buf)->data()[0];
    REQUIRE(afterFail == customOut);

    std::error_code ec;
    std::filesystem::remove(iccPath, ec);
}

TEST_CASE("ColorManager: sRGBEncodedToProfile converts pixels into the target ICC space",
          "[color][p1]") {
    // R-03 regression: exporting to a non-sRGB ICC must actually transform the
    // pixels into that space (not just embed the tag). Verify by round-tripping
    // the converted output back from AdobeRGB to sRGB with an independent test-side
    // lcms transform — if the pixels had been left in sRGB, the second leg would be
    // a no-op and the source colour would not be recovered closely.
    ColorManager cm;
    const auto adobeBytes = ColorManager::exportProfileBytes(
        ColorManager::ExportColorProfile::AdobeRGB);
    REQUIRE_FALSE(adobeBytes.empty());

    // A mid-saturation colour comfortably inside the AdobeRGB gamut.
    const uint8_t r = 200, g = 120, b = 60;
    auto src = ImageBuffer::create(1, 1, PixelFormat::U8);
    src->data()[0] = r;
    src->data()[1] = g;
    src->data()[2] = b;

    auto out = cm.sRGBEncodedToProfile(*src, adobeBytes);
    REQUIRE(out->width() == 1);
    REQUIRE(out->format() == PixelFormat::U8);
    const uint8_t* p = out->data().data();
    // A genuine conversion must move at least one channel; otherwise the pixels
    // were left in sRGB while the file would be tagged AdobeRGB.
    const bool unchanged = (p[0] == r && p[1] == g && p[2] == b);
    REQUIRE_FALSE(unchanged);

    // Recover sRGB from the converted AdobeRGB output and compare to the source.
    cmsHPROFILE srcP = cmsOpenProfileFromMem(
        adobeBytes.data(), static_cast<cmsUInt32Number>(adobeBytes.size()));
    cmsHPROFILE dstP = cmsCreate_sRGBProfile();
    REQUIRE(srcP);
    REQUIRE(dstP);
    cmsHTRANSFORM tf = cmsCreateTransform(
        srcP, TYPE_RGB_8, dstP, TYPE_RGB_8, INTENT_RELATIVE_COLORIMETRIC,
        cmsFLAGS_HIGHRESPRECALC | cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE);
    REQUIRE(tf);
    uint8_t back[3];
    cmsDoTransform(tf, p, back, 1);
    cmsDeleteTransform(tf);
    cmsCloseProfile(dstP);
    cmsCloseProfile(srcP);

    // The sRGB<->AdobeRGB round-trip is visually lossless in the mid-tones; a
    // tight tolerance confirms the pixels really were transformed and recovered.
    REQUIRE(std::abs(int(back[0]) - int(r)) <= 3);
    REQUIRE(std::abs(int(back[1]) - int(g)) <= 3);
    REQUIRE(std::abs(int(back[2]) - int(b)) <= 3);
}
