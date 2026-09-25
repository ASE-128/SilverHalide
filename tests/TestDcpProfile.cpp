#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "engine/DcpProfile.h"

static void writeBytes(const std::filesystem::path& path,
                       const std::vector<uint8_t>& bytes) {
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    REQUIRE(f != nullptr);
    const std::size_t written =
        std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    REQUIRE(written == bytes.size());
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
    // read the shifted pair 0x7812 instead of 0x5678).
    REQUIRE(profile->toneCurve[0] == 0x1234);
    REQUIRE(profile->toneCurve[1] == 0x5678);
    REQUIRE(profile->toneCurve[2] == 0x9ABC);
    REQUIRE(profile->toneCurve[3] == 0xDEF0);
}
