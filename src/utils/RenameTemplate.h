#pragma once

// P1-3: batch-rename template rendering. Pure C++ (no Qt) so the renderer can
// be unit-tested headless and shared by the import/export dialogs (UI) and the
// catalog rename worker.
//
// Template tokens (case-insensitive):
//   {date}         -> the capture date string supplied by the caller
//                     (e.g. "2026-09-05" or "20260905" — passed straight through)
//   {index[:W]}    -> zero-padded sequence number; `:W` sets the field width
//                     (default 2). e.g. {index:4} gives "0001"
//   {base}         -> the original file's base name (without extension)
//
// Unknown tokens are left as their literal text so a typo degrades gracefully
// instead of producing an empty name.

#include <cstdio>
#include <filesystem>
#include <string>

namespace lrc {

struct RenameTemplateArgs {
    std::string tmpl;          // the template, e.g. "{date}_{base}_{index:3}"
    std::string base;          // original base name (no extension)
    int index = 1;             // 1-based sequence number
    int indexWidth = 2;        // minimum zero-padding for {index}
    std::string date;          // capture date (passed through verbatim)
};

/** Renders `args.tmpl` into a file base name (no extension). */
inline std::string renderRenameTemplate(const RenameTemplateArgs& args) {
    // Lowercase the template for case-insensitive token matching.
    std::string t = args.tmpl;

    // Zero-pad the index into a string once (appears in all substitutions).
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*d", args.indexWidth, args.index);
    const std::string padded = buf;

    std::string out;
    out.reserve(t.size() + 16);
    const std::string lower = [&t] {
        std::string s = t;
        for (auto& c : s)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>((c - 'A') + 'a');
        return s;
    }();

    size_t i = 0;
    const size_t n = lower.size();
    while (i < n) {
        if (t[i] != '{') {
            out.push_back(t[i++]);
            continue;
        }
        // Try to match a token beginning at i.
        const size_t close = lower.find('}', i);
        if (close == std::string::npos) {
            out.push_back(t[i++]);
            continue;
        }
        const std::string token = t.substr(i, close - i + 1);
        const std::string lowToken = lower.substr(i, close - i + 1);
        if (lowToken == "{date}") {
            out += args.date;
            i = close + 1;
        } else if (lowToken == "{base}") {
            out += args.base;
            i = close + 1;
        } else if (lowToken.rfind("{index", 0) == 0) {
            // {index} or {index:W}
            int width = args.indexWidth;
            std::string inner = lowToken.substr(2, lowToken.size() - 3); // "index[:W]"
            const size_t colon = inner.find(':');
            if (colon != std::string::npos) {
                const int w = std::atoi(inner.c_str() + colon + 1);
                if (w > 0)
                    width = w;
            }
            if (width > 0) {
                char b[32];
                std::snprintf(b, sizeof(b), "%.*d", width, args.index);
                out += b;
            } else {
                out += padded;
            }
            i = close + 1;
        } else {
            (void)token;
            out.push_back(t[i++]);
        }
    }
    return out;
}

/**
 * @brief Renders a rename template into a unique path inside `dir`, keeping the
 *        original extension and appending "-N" before it until no on-disk name
 *        collides (mirrors LibraryManager's conflict policy).
 *
 * The date/index/base come from `args`; `ext` is the file extension including
 * the leading dot. Deterministic for a given input, so the import dialog's
 * in-memory preview and the worker's on-disk rename agree on the base name;
 * only the actual collision resolution is inferred from the filesystem here.
 *
 * @param dir  Target directory (must exist for uniqueness checks to be meaningful).
 * @param args Template + base + index + date to render.
 * @param ext  Extension verbatim, e.g. ".dng".
 * @return The first free path with the rendered name (or "-N" suffixes as needed).
 */
inline std::filesystem::path uniqueRenamePath(
    const std::filesystem::path& dir, const RenameTemplateArgs& args,
    const std::string& ext) {
    std::error_code ec;
    const std::filesystem::path first = dir / (renderRenameTemplate(args) + ext);
    if (!std::filesystem::exists(first)) {
        std::filesystem::create_directories(dir, ec);
        return first;
    }
    for (int i = 2; i < 100000; ++i) {
        const std::filesystem::path candidate =
            dir / (renderRenameTemplate(args) + "-" + std::to_string(i) + ext);
        if (!std::filesystem::exists(candidate)) {
            std::filesystem::create_directories(dir, ec);
            return candidate;
        }
    }
    return dir / (renderRenameTemplate(args) + "-overflow" + ext);
}

} // namespace lrc