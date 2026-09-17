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

/**
 * @brief SHA-256 of a byte buffer.
 *
 * @param data Input bytes.
 * @param len  Number of bytes to hash; 0 (or a null `data`) hashes the empty
 *             message.
 * @return 64 lowercase hex chars.
 */
std::string sha256Buffer(const uint8_t* data, size_t len);

/**
 * @brief SHA-256 of the file at `path`.
 *
 * Reads the file in 16 KB chunks so multi-hundred-MB model files are never
 * loaded fully into memory.
 *
 * @param path File to hash.
 * @return 64 lowercase hex chars, or an empty string if the file cannot be
 *         opened.
 */
std::string sha256File(const std::filesystem::path& path);

}  // namespace lrc
