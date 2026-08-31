#pragma once

// Self-contained SHA-256 helpers shared by the engine and app layers.
// Lives in the Qt-free utils layer so it can be unit-tested headlessly
// (tests/TestSha256.cpp) — the previous in-ModelManager implementation had
// no headless coverage and a remainder-handling bug went undetected.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace lrc {

// SHA-256 of a byte buffer, returned as 64 lowercase hex chars.
std::string sha256Buffer(const uint8_t* data, size_t len);

// SHA-256 of a file at `path`, returned as 64 lowercase hex chars.
// Returns an empty string if the file cannot be opened. Disk-based
// (16 KB chunks) so multi-hundred-MB model files don't load into memory.
std::string sha256File(const std::filesystem::path& path);

}  // namespace lrc
