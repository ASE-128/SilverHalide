#pragma once

// Polling folder watcher (std::filesystem based — Qt-free). The UI drives
// poll() from a QTimer; changed files are reported through a callback. Phase-1
// import watches the imported folder so newly added files appear automatically.

#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace lrc {

class FolderMonitor {
public:
    using ChangeCallback =
        std::function<void(const std::vector<std::filesystem::path>& added,
                           const std::vector<std::filesystem::path>& removed)>;

    explicit FolderMonitor(std::filesystem::path folder);

    void setCallback(ChangeCallback callback);

    // Records the current file set as the baseline (called on import).
    void start();

    // Scans the folder and reports differences since the last call.
    void poll();

    const std::vector<std::filesystem::path>& currentFiles() const;

    // Extension lists and path predicates delegate to engine/ImageFormats,
    // the single source of truth shared with RawDecoder and the GUI dialogs.
    static const std::vector<std::string>& rawExtensions();
    // rawExtensions() plus JPEG/PNG/TIFF (the formats the app can export).
    static const std::vector<std::string>& importExtensions();

    static bool isRawFile(const std::filesystem::path& path);
    static bool isImportableFile(const std::filesystem::path& path);

private:
    std::filesystem::path folder_;
    ChangeCallback callback_;
    std::set<std::filesystem::path> known_;
    std::vector<std::filesystem::path> snapshot_;
    mutable std::mutex mutex_;
};

} // namespace lrc
