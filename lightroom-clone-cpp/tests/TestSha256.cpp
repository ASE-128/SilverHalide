// Regression test for utils/Sha256.
//
// This exists because the previous in-ModelManager SHA-256 implementation had
// a buffer-remainder bug: for any input whose last chunk left a non-zero
// remainder in the 64-byte block buffer (i.e. every real model file >64 B),
// a bogus first memcpy wrote the remainder at a wrapped (negative) offset,
// corrupting the context's `count`/`state` fields and producing a wrong hash.
// The bug went undetected because the only model the headless suite exercised
// (test_add_2x2) uses a placeholder SHA that is skipped at verify time.
//
// These vectors are computed with .NET's SHA256 (System.Security.Cryptography)
// and cross-checked against certutil. The 100/1000-byte cases specifically
// exercise the multi-block + non-zero remainder path that was broken.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "utils/Sha256.h"

using namespace lrc;

static std::vector<uint8_t> bytesOf(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

static std::string repeat(const std::string& s, size_t n) {
    std::string out;
    out.reserve(s.size() * n);
    for (size_t i = 0; i < n; ++i) out += s;
    return out;
}

TEST_CASE("Sha256: empty input", "[sha256]") {
    auto h = sha256Buffer(nullptr, 0);
    REQUIRE(h == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("Sha256: empty via zero-length buffer", "[sha256]") {
    auto b = bytesOf("");
    auto h = sha256Buffer(b.data(), b.size());
    REQUIRE(h == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("Sha256: 'abc' (3 bytes, single block + remainder)", "[sha256]") {
    auto b = bytesOf("abc");
    REQUIRE(sha256Buffer(b.data(), b.size()) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("Sha256: 100 'a' (1 full block + 36-byte remainder)", "[sha256]") {
    // This is the minimal case that caught the remainder bug: 64 bytes fill
    // exactly one block (consumed by transform), leaving 36 bytes in the
    // remainder path — the path the bogus first memcpy corrupted.
    const auto s = repeat("a", 100);
    auto b = bytesOf(s);
    REQUIRE(sha256Buffer(b.data(), b.size()) ==
            "2816597888e4a0d3a36b82b83316ab32680eb8f00f8cd3b904d681246d285a0e");
}

TEST_CASE("Sha256: 1000 'a' (15 full blocks + 40-byte remainder)", "[sha256]") {
    // Exercises multiple transform() calls across update() invocations plus
    // a non-zero final remainder — the exact shape of a multi-hundred-MB
    // model file read in 16 KB chunks (each chunk = 256 full blocks).
    const auto s = repeat("a", 1000);
    auto b = bytesOf(s);
    REQUIRE(sha256Buffer(b.data(), b.size()) ==
            "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

TEST_CASE("Sha256: file path matches buffer path", "[sha256]") {
    // Round-trip: sha256File of a temp file must equal sha256Buffer of the
    // same bytes. Uses the 100-'a' case so the remainder path is exercised
    // through the 16 KB-chunked file reader too.
    const auto s = repeat("a", 100);
    auto b = bytesOf(s);
    const auto path = std::filesystem::temp_directory_path() / "lrc_sha256_test.bin";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        REQUIRE(f);
        f.write(reinterpret_cast<const char*>(b.data()),
                static_cast<std::streamsize>(b.size()));
    }
    const auto fromFile = sha256File(path);
    const auto fromBuf  = sha256Buffer(b.data(), b.size());
    std::error_code ec;
    std::filesystem::remove(path, ec);
    REQUIRE(fromFile == fromBuf);
    REQUIRE(fromFile ==
            "2816597888e4a0d3a36b82b83316ab32680eb8f00f8cd3b904d681246d285a0e");
}

TEST_CASE("Sha256: missing file returns empty string", "[sha256]") {
    REQUIRE(sha256File("definitely_does_not_exist_lrc_sha256.bin").empty());
}
