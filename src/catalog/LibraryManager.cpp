#include "catalog/LibraryManager.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <ctime>
#include <cwctype>
#include <string>

namespace lrc {

namespace {

// Case-insensitive path-component equality (Windows filesystems are
// case-insensitive, but std::filesystem comparisons are not).
bool componentEquals(const std::filesystem::path& a,
                     const std::filesystem::path& b) {
    std::wstring wa = a.wstring();
    std::wstring wb = b.wstring();
    for (wchar_t& c : wa)
        c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
    for (wchar_t& c : wb)
        c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
    return wa == wb;
}

// True when `path` is `root` itself or sits somewhere under it.
bool isUnder(const std::filesystem::path& path,
             const std::filesystem::path& root) {
    // Keep the normalized temporaries alive: their iterators are only valid
    // while the path objects they belong to exist.
    const auto normPath = path.lexically_normal();
    const auto normRoot = root.lexically_normal();
    auto it = normPath.begin();
    for (auto rit = normRoot.begin(); rit != normRoot.end(); ++rit, ++it) {
        if (it == normPath.end())
            return false;
        if (!componentEquals(*it, *rit))
            return false;
    }
    return true;
}

// Modification time of `src` as a local calendar date (fall back to now when
// the filesystem query fails).
void sourceDate(const std::filesystem::path& src, int& year, int& month,
                int& day) {
    using namespace std::chrono;
    std::error_code ec;
    auto ft = std::filesystem::last_write_time(src, ec);
    std::time_t t;
    if (ec) {
        t = system_clock::to_time_t(system_clock::now());
    } else {
        const auto sct = time_point_cast<system_clock::duration>(
            ft - file_clock::now() + system_clock::now());
        t = system_clock::to_time_t(sct);
    }
    std::tm tm{};
    localtime_s(&tm, &t);
    year = tm.tm_year + 1900;
    month = tm.tm_mon + 1;
    day = tm.tm_mday;
}

void setError(std::string* error, const std::string& message) {
    if (error)
        *error = message;
}

} // namespace

std::filesystem::path LibraryManager::suggestDestPath(
    const std::filesystem::path& src,
    const std::filesystem::path& libraryRoot) {
    if (isUnder(src, libraryRoot))
        return src.lexically_normal();

    int year = 0, month = 0, day = 0;
    sourceDate(src, year, month, day);

    char dateBuf[16];
    std::snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", year, month,
                  day);
    const auto baseDir = libraryRoot.lexically_normal() /
                         std::to_string(year) / dateBuf;

    const auto plain = baseDir / src.filename();
    if (!std::filesystem::exists(plain)) {
        std::error_code ec;
        std::filesystem::create_directories(baseDir, ec);
        return plain;
    }

    // The name is taken (or the file is about to be overwritten): append a
    // numeric suffix so both files are preserved.
    const std::string stem = src.stem().string();
    const std::string ext = src.extension().string();
    for (int i = 2; i < 1000; ++i) {
        const auto candidate =
            baseDir / (stem + "-" + std::to_string(i) + ext);
        if (!std::filesystem::exists(candidate)) {
            std::error_code ec;
            std::filesystem::create_directories(baseDir, ec);
            return candidate;
        }
    }
    return baseDir / (stem + "-9999" + ext);
}

std::optional<std::filesystem::path> LibraryManager::importFile(
    const std::filesystem::path& src,
    const std::filesystem::path& libraryRoot, ImportMode mode,
    std::string* error) {
    if (mode == ImportMode::Add || isUnder(src, libraryRoot))
        return src.lexically_normal();

    const auto dest = suggestDestPath(src, libraryRoot);
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    if (ec) {
        setError(error, "create directories failed: " + dest.parent_path().string());
        return std::nullopt;
    }

    std::filesystem::copy_file(src, dest,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
        setError(error, "copy failed for " + src.string() + ": " + ec.message());
        return std::nullopt;
    }

    if (mode == ImportMode::Move) {
        // std::filesystem::remove reports failure through ec; a false return
        // with a clean ec simply means the source was already absent, which is
        // a completed move, not an error. On a real failure the copy stays in
        // the library and the caller is told the source may remain on disk
        // (M4) -- importFile still returns the destination so the copy is
        // cataloged, but the error string must not be swallowed.
        std::filesystem::remove(src, ec);
        if (ec)
            setError(error, "copied to " + dest.string() +
                               " but could not remove the source: " +
                               ec.message());
    }
    return dest.lexically_normal();
}

} // namespace lrc
