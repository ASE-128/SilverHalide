#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "catalog/LibraryManager.h"

using namespace lrc;

namespace {

std::filesystem::path tempDir() {
    static size_t counter = 0;
    const auto dir =
        std::filesystem::temp_directory_path() /
        ("lrc_library_test_" + std::to_string(++counter) + "_" +
         std::to_string(static_cast<unsigned long>(
             std::chrono::system_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(dir);
    return dir;
}

std::filesystem::path makeFile(const std::filesystem::path& dir,
                               const std::string& name,
                               const std::string& content = "x") {
    std::filesystem::create_directories(dir);
    const auto p = dir / name;
    std::ofstream(p) << content;
    return p;
}

// Case-insensitive "path sits under root" (Windows-style semantics).
bool isUnder(const std::filesystem::path& path,
             const std::filesystem::path& root) {
    const auto normPath = path.lexically_normal();
    const auto normRoot = root.lexically_normal();
    auto it = normPath.begin();
    for (auto rit = normRoot.begin(); rit != normRoot.end(); ++rit, ++it) {
        if (it == normPath.end())
            return false;
        std::wstring a = it->wstring();
        std::wstring b = rit->wstring();
        for (wchar_t& c : a)
            c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
        for (wchar_t& c : b)
            c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
        if (a != b)
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("LibraryManager: suggestDestPath nests under a dated folder",
          "[catalog]") {
    const auto root = tempDir() / "Library";
    const auto src = makeFile(tempDir() / "source", "IMG_0001.arw");

    const auto dest = LibraryManager::suggestDestPath(src, root);
    // Shape: <root>/<yyyy>/<yyyy-mm-dd>/<filename>.
    const auto rel = dest.lexically_relative(root);
    REQUIRE(rel.has_filename());
    std::vector<std::filesystem::path> parts;
    for (const auto& part : rel)
        parts.push_back(part);
    REQUIRE(parts.size() == 3);
    const std::string yearStr = parts[0].string();
    REQUIRE(yearStr.size() == 4);
    REQUIRE(std::all_of(yearStr.begin(), yearStr.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; }));
    const std::string date = parts[1].string();
    REQUIRE(date.size() == 10);
    REQUIRE(date[4] == '-');
    REQUIRE(date[7] == '-');
    REQUIRE(parts[2] == "IMG_0001.arw");
    REQUIRE(isUnder(dest, root));
}

TEST_CASE("LibraryManager: suggestDestPath suffixes a duplicate name",
          "[catalog]") {
    const auto root = tempDir() / "Library";
    const auto src = makeFile(tempDir() / "source", "IMG_0002.dng");
    const auto dest1 = LibraryManager::suggestDestPath(src, root);
    // Occupy the plain destination, then the -2 variant.
    makeFile(dest1.parent_path(), dest1.filename().string());
    makeFile(dest1.parent_path(), "IMG_0002-2.dng");

    REQUIRE(LibraryManager::suggestDestPath(src, root) ==
            dest1.parent_path() / "IMG_0002-3.dng");
}

TEST_CASE("LibraryManager: files already in the library return unchanged",
          "[catalog]") {
    const auto root = tempDir() / "Library";
    const auto inside = makeFile(root / "2026" / "2026-08-11", "keep.jpg");
    REQUIRE(LibraryManager::suggestDestPath(inside, root) == inside);
    const auto res = LibraryManager::importFile(inside, root, ImportMode::Move);
    REQUIRE(res.has_value());
    REQUIRE(*res == inside);
}

TEST_CASE("LibraryManager: Add leaves the source untouched", "[catalog]") {
    const auto root = tempDir() / "Library";
    const auto src = makeFile(tempDir() / "source", "a.jpg");
    const auto res = LibraryManager::importFile(src, root, ImportMode::Add);
    REQUIRE(res.has_value());
    REQUIRE(*res == src);
    REQUIRE(std::filesystem::exists(src));
}

TEST_CASE("LibraryManager: Copy duplicates into the library", "[catalog]") {
    const auto root = tempDir() / "Library";
    const auto src = makeFile(tempDir() / "source", "b.arw", "raw-bytes");
    const auto res = LibraryManager::importFile(src, root, ImportMode::Copy);
    REQUIRE(res.has_value());
    REQUIRE(std::filesystem::exists(src));
    REQUIRE(std::filesystem::exists(*res));
    REQUIRE(isUnder(*res, root));
    REQUIRE(std::filesystem::file_size(src) == std::filesystem::file_size(*res));
}

TEST_CASE("LibraryManager: Move relocates and removes the source",
          "[catalog]") {
    const auto root = tempDir() / "Library";
    const auto src = makeFile(tempDir() / "source", "c.tiff", "tiff-bytes");
    const auto res = LibraryManager::importFile(src, root, ImportMode::Move);
    REQUIRE(res.has_value());
    REQUIRE_FALSE(std::filesystem::exists(src));
    REQUIRE(std::filesystem::exists(*res));
    REQUIRE(std::filesystem::file_size(*res) == 10);
}
