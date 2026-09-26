#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include "engine/DcpProfile.h"
#include "engine/ImageBuffer.h"
#include "engine/RawDecoder.h"

static void writeBytes(const std::filesystem::path& path,
                       const std::vector<uint8_t>& bytes) {
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    REQUIRE(f != nullptr);
    const std::size_t written =
        std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    REQUIRE(written == bytes.size());
}

// Builds a minimal little-endian DCP whose hue/sat grid is filled with one
// constant (dh, ds, dv) triplet. Layout: 8-byte header, IFD with 4 entries
// (ProfileName + ToneCurve inline, dims and map data appended after the IFD),
// matching the tag numbers Adobe DCP files use (50981/50982).
static std::vector<uint8_t> buildConstMapDcp(uint32_t hueDiv, uint32_t satDiv,
                                             uint32_t valDiv, float dh,
                                             float ds, float dv) {
    const uint32_t cells = hueDiv * satDiv * valDiv;
    std::vector<uint8_t> b;
    auto u16 = [&b](uint16_t v) {
        b.push_back(static_cast<uint8_t>(v & 0xFF));
        b.push_back(static_cast<uint8_t>(v >> 8));
    };
    auto u32 = [&b](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    };
    auto f32 = [&b, &u32](float f) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        u32(u);
    };
    b.push_back('I');
    b.push_back('I');
    u16(42);
    u32(8);
    u16(4);  // 4 IFD entries
    const uint32_t dimsOff = 8 + 2 + 4 * 12 + 4;
    const uint32_t mapOff = dimsOff + 12;
    // ProfileName 50936: ASCII "T\0", inline.
    u16(50936); u16(2); u32(2); b.push_back('T'); b.push_back(0); u16(0);
    // ToneCurve 50940: two SHORT samples (0, 65535), inline.
    u16(50940); u16(3); u32(2); u16(0); u16(65535);
    // ProfileHueSatMapDims 50981: three LONGs.
    u16(50981); u16(4); u32(3); u32(dimsOff);
    // ProfileHueSatMapData1 50982: cells * 3 floats.
    u16(50982); u16(11); u32(cells * 3); u32(mapOff);
    u32(0);  // next IFD
    u32(hueDiv); u32(satDiv); u32(valDiv);
    for (uint32_t i = 0; i < cells; ++i) {
        f32(dh);
        f32(ds);
        f32(dv);
    }
    return b;
}

TEST_CASE("DCP rejects missing and non-TIFF files", "[dcp]") {
    REQUIRE_FALSE(lrc::DcpProfileReader::isDcp(
        std::filesystem::temp_directory_path() / "does-not-exist.dcp"));
    REQUIRE_FALSE(lrc::DcpProfileReader::read(
        std::filesystem::temp_directory_path() / "does-not-exist.dcp"));
}

TEST_CASE("DCP reader loads an Adobe camera profile", "[dcp]") {
    const auto path = std::filesystem::path(LRC_SOURCE_ROOT) / "resources" /
        "Camera" / "Canon EOS 1000D" /
        "Canon EOS 1000D Camera Faithful.dcp";
    REQUIRE(std::filesystem::exists(path));
    REQUIRE(lrc::DcpProfileReader::isDcp(path));
    const auto profile = lrc::DcpProfileReader::read(path);
    REQUIRE(profile.has_value());
    REQUIRE(profile->name == "Camera Faithful");
    REQUIRE(profile->cameraModel == "Canon EOS 1000D");
    REQUIRE(profile->hasColorMatrix1);
    REQUIRE(profile->hasColorMatrix2);
    REQUIRE(profile->toneCurveSize > 0);
    // Dual-illuminant calibration: Standard A + D65, pairing with the two
    // colour matrices (and the two hue/sat maps when both are present).
    REQUIRE(profile->hasCalibrationIlluminant1);
    REQUIRE(profile->hasCalibrationIlluminant2);
    REQUIRE(profile->calibrationIlluminant[0] == 17.0f);
    REQUIRE(profile->calibrationIlluminant[1] == 21.0f);
    // The hue/sat map is the bulk of the file and must parse whole: grid
    // dimensions valid and the table length exactly dims*3 floats.
    REQUIRE(profile->hasHueSatMap1);
    REQUIRE(profile->hueSatDims[0] >= 1);
    REQUIRE(profile->hueSatDims[1] >= 2);
    REQUIRE(profile->hueSatDims[2] >= 1);
    const size_t cells = static_cast<size_t>(profile->hueSatDims[0]) *
                         profile->hueSatDims[1] * profile->hueSatDims[2];
    REQUIRE(profile->hueSatMap1.size() == cells * 3);
    // No ProfileHueSatMapEncoding tag in Adobe camera profiles: linear
    // indexing (0) is the spec default.
    REQUIRE(profile->hueSatMapEncoding == 0);
}

TEST_CASE("DCP dual-illuminant matrix stays within its calibrations",
          "[dcp]") {
    const auto path = std::filesystem::path(LRC_SOURCE_ROOT) / "resources" /
        "Camera" / "Canon EOS 1000D" /
        "Canon EOS 1000D Camera Faithful.dcp";
    const auto profile = lrc::DcpProfileReader::read(path);
    REQUIRE(profile.has_value());
    const auto m = lrc::RawDecoder::dcpColorMatrix(*profile, {1.0, 1.0, 1.0});
    // The interpolated matrix must be elementwise between ColorMatrix1 and
    // ColorMatrix2; anything outside means the illuminant weight escaped
    // its [0,1] clamp.
    for (size_t i = 0; i < m.size(); ++i) {
        const float lo = std::min(profile->colorMatrix1[i], profile->colorMatrix2[i]);
        const float hi = std::max(profile->colorMatrix1[i], profile->colorMatrix2[i]);
        REQUIRE(m[i] >= lo - 1e-6f);
        REQUIRE(m[i] <= hi + 1e-6f);
    }
}

TEST_CASE("DCP hue/sat map identity leaves pixels unchanged", "[dcp]") {
    // 3D grid (value divisions > 1) to exercise the full trilinear path.
    const auto bytes = buildConstMapDcp(4, 2, 2, 0.0f, 1.0f, 1.0f);
    const auto path = std::filesystem::temp_directory_path() /
        "lrc_identity_huesat.dcp";
    writeBytes(path, bytes);
    const auto profile = lrc::DcpProfileReader::read(path);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    REQUIRE(profile.has_value());
    REQUIRE(profile->hasHueSatMap1);

    auto buf = lrc::ImageBuffer::create(3, 1, lrc::PixelFormat::F32);
    buf->setPixel(0, 0, 0, 0.25f);
    buf->setPixel(0, 0, 1, 0.50f);
    buf->setPixel(0, 0, 2, 0.75f);
    buf->setPixel(1, 0, 0, 1.0f);
    buf->setPixel(1, 0, 1, 0.0f);
    buf->setPixel(1, 0, 2, 0.0f);
    buf->setPixel(2, 0, 0, 0.0f);
    buf->setPixel(2, 0, 1, 1.0f);
    buf->setPixel(2, 0, 2, 0.0f);

    const auto out =
        lrc::RawDecoder::applyDcpHueSatMap(buf, *profile, {1.0, 1.0, 1.0});
    for (int x = 0; x < 3; ++x)
        for (int c = 0; c < 3; ++c)
            REQUIRE(out->pixel(x, 0, c) ==
                    Catch::Approx(buf->pixel(x, 0, c)).margin(1e-5));
}

TEST_CASE("DCP hue/sat map applies hue, saturation and value deltas",
          "[dcp]") {
    const auto path = std::filesystem::temp_directory_path() /
        "lrc_delta_huesat.dcp";

    // Hue shift: +90 degrees rotates red to chartreuse (0.5, 1, 0) in HSV.
    writeBytes(path, buildConstMapDcp(4, 2, 1, 90.0f, 1.0f, 1.0f));
    auto profile = lrc::DcpProfileReader::read(path);
    REQUIRE(profile.has_value());
    auto buf = lrc::ImageBuffer::create(1, 1, lrc::PixelFormat::F32);
    buf->setPixel(0, 0, 0, 1.0f);
    buf->setPixel(0, 0, 1, 0.0f);
    buf->setPixel(0, 0, 2, 0.0f);
    auto out = lrc::RawDecoder::applyDcpHueSatMap(buf, *profile, {1.0, 1.0, 1.0});
    REQUIRE(out->pixel(0, 0, 1) == Catch::Approx(1.0f).margin(1e-5));
    REQUIRE(out->pixel(0, 0, 2) == Catch::Approx(0.0f).margin(1e-5));
    REQUIRE(out->pixel(0, 0, 0) == Catch::Approx(0.5f).margin(1e-5));

    // Saturation scale: halving (1, 0.5, 0.5) pulls it halfway to its value
    // axis, giving (1, 0.75, 0.75).
    writeBytes(path, buildConstMapDcp(4, 2, 1, 0.0f, 0.5f, 1.0f));
    profile = lrc::DcpProfileReader::read(path);
    REQUIRE(profile.has_value());
    buf->setPixel(0, 0, 0, 1.0f);
    buf->setPixel(0, 0, 1, 0.5f);
    buf->setPixel(0, 0, 2, 0.5f);
    out = lrc::RawDecoder::applyDcpHueSatMap(buf, *profile, {1.0, 1.0, 1.0});
    REQUIRE(out->pixel(0, 0, 0) == Catch::Approx(1.0f).margin(1e-5));
    REQUIRE(out->pixel(0, 0, 1) == Catch::Approx(0.75f).margin(1e-5));
    REQUIRE(out->pixel(0, 0, 2) == Catch::Approx(0.75f).margin(1e-5));

    // Value scale: a global 0.5 multiplier dims pure red to half.
    writeBytes(path, buildConstMapDcp(4, 2, 1, 0.0f, 1.0f, 0.5f));
    profile = lrc::DcpProfileReader::read(path);
    REQUIRE(profile.has_value());
    buf->setPixel(0, 0, 0, 1.0f);
    buf->setPixel(0, 0, 1, 0.0f);
    buf->setPixel(0, 0, 2, 0.0f);
    out = lrc::RawDecoder::applyDcpHueSatMap(buf, *profile, {1.0, 1.0, 1.0});
    REQUIRE(out->pixel(0, 0, 0) == Catch::Approx(0.5f).margin(1e-5));
    REQUIRE(out->pixel(0, 0, 1) == Catch::Approx(0.0f).margin(1e-5));
    REQUIRE(out->pixel(0, 0, 2) == Catch::Approx(0.0f).margin(1e-5));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("DCP hue/sat map is a no-op without a map", "[dcp]") {
    lrc::DcpProfile profile;
    auto buf = lrc::ImageBuffer::create(1, 1, lrc::PixelFormat::F32);
    const auto out =
        lrc::RawDecoder::applyDcpHueSatMap(buf, profile, {1.0, 1.0, 1.0});
    REQUIRE(out == buf);
}

// Regression: ToneCurve(50940) stored as UNDEFINED(7) carries 16-bit samples.
// A bug previously stepped the reader by 1 byte (typeSize(7)==1) while reading
// 2 bytes per sample, interleaving the sample stream. Sample 1 must equal the
// second 16-bit little-endian value written to the file (0x5678), not the
// byte-shifted garbage (0x7812) the bug produced.
TEST_CASE("DCP UNDEFINED tone curve is read at 16-bit stride", "[dcp]") {
    // Minimal TIFF/DCP: II header, magic 42, IFD at 8 with 2 entries.
    const std::vector<uint8_t> bytes = {
        0x49, 0x49, 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00, // header, IFD off=8
        0x02, 0x00,                                    // IFD: 2 entries
        // Entry 0: ProfileName 50936 (0xC6F8), ASCII, offset 38 (0x26)
        0xF8, 0xC6, 0x02, 0x00, 0x0D, 0x00, 0x00, 0x00, 0x26, 0x00, 0x00, 0x00,
        // Entry 1: ToneCurve 50940 (0xC6FC), type 7 UNDEFINED, count=8 bytes, offset 51 (0x33)
        0xFC, 0xC6, 0x07, 0x00, 0x08, 0x00, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00,
        // Next IFD offset = 0
        0x00, 0x00, 0x00, 0x00,
        // offset 38: ProfileName string "Test Profile\0" (13 bytes)
        0x54, 0x65, 0x73, 0x74, 0x20, 0x50, 0x72, 0x6F, 0x66, 0x69, 0x6C, 0x65, 0x00,
        // offset 51: ToneCurve data, 4x 16-bit LE samples
        //   0x1234, 0x5678, 0x9ABC, 0xDEF0
        0x34, 0x12, 0x78, 0x56, 0xBC, 0x9A, 0xF0, 0xDE,
    };
    const auto path = std::filesystem::temp_directory_path() / "lrc_undefined_tonecurve.dcp";
    writeBytes(path, bytes);
    const auto profile = lrc::DcpProfileReader::read(path);
    std::remove(path.string().c_str());
    REQUIRE(profile.has_value());
    REQUIRE(profile->toneCurveSize > 0);
    // First four in-range samples decode to the intended 16-bit values; this
    // is exactly where the old 1-byte stride interleaved the stream (sample 1
    // read the shifted pair 0x7812 instead of 0x5678). Integer-typed curves
    // are normalized into the F32 working range at read time.
    REQUIRE(profile->toneCurve[0] == Catch::Approx(0x1234 / 65535.0f).margin(1e-7));
    REQUIRE(profile->toneCurve[1] == Catch::Approx(0x5678 / 65535.0f).margin(1e-7));
    REQUIRE(profile->toneCurve[2] == Catch::Approx(0x9ABC / 65535.0f).margin(1e-7));
    REQUIRE(profile->toneCurve[3] == Catch::Approx(0xDEF0 / 65535.0f).margin(1e-7));
}
