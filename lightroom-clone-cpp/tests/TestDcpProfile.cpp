#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "engine/DcpProfile.h"

TEST_CASE("DCP reader rejects missing and non-TIFF files", "[dcp]") {
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
