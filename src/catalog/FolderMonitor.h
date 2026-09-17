#pragma once

/**
 * @brief Polling folder watcher (std::filesystem based — Qt-free).
 *
 * The UI drives poll() from a QTimer; changes are reported through a callback.
 * Phase-1 import watches the imported folder so newly added files appear
 * automatically. Beyond path add/remove the monitor also tracks file identity
 * (size + last-write time) so an in-place content change is reported as
 * "modified". Removable files are only reported after they stay gone across
 * kOfflineThreshold consecutive polls; when the whole folder becomes
 * unavailable in a single poll no removal is reported at all, so a transient
 * network/disconnected-drive absence cannot destroy catalog records.
 */

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace lrc {

class FolderMonitor {
public:
    /** @brief Result of one poll() scan.
     *
     *  `modified` = files whose size or last-write time changed since the
     *  previous scan (content replaced in place). `removed` is only emitted
     *  after a file has been absent for kOfflineThreshold consecutive polls. */
    using ChangeCallback =
        std::function<void(const std::vector<std::filesystem::path>& added,
                           const std::vector<std::filesystem::path>& modified,
                           const std::vector<std::filesystem::path>& removed)>;

    explicit FolderMonitor(std::filesystem::path folder);

    /// @brief Installs the change callback; replaces any previous one.
    void setCallback(ChangeCallback callback);

    /// @brief Records the current file set as the baseline (called on import).
    void start();

    /// @brief Scans the folder and reports differences since the last call.
    void poll();

    /** @brief Current known importable file set.
     *  @return Live snapshot refreshed by start()/poll(); valid until the next
     *          poll. */
    const std::vector<std::filesystem::path>& currentFiles() const;

    // Extension lists and path predicates delegate to engine/ImageFormats,
    // the single source of truth shared with RawDecoder and the GUI dialogs.
    static const std::vector<std::string>& rawExtensions();
    // rawExtensions() plus JPEG/PNG/TIFF (the formats the app can export).
    static const std::vector<std::string>& importExtensions();

    static bool isRawFile(const std::filesystem::path& path);
    static bool isImportableFile(const std::filesystem::path& path);

private:
    // File identity used to detect in-place content changes.
    struct FileStamp {
        std::uintmax_t size = 0;
        std::filesystem::file_time_type modified{};
        bool operator==(const FileStamp&) const = default;
    };

    std::filesystem::path folder_;
    ChangeCallback callback_;
    // Known importable files -> their last observed identity.
    std::map<std::filesystem::path, FileStamp> known_;
    // Consecutive times each known-but-missing file was absent (offline
    // grace). A file is not reported as removed until this reaches the
    // threshold.
    std::map<std::filesystem::path, int> missingPolls_;
    // How many consecutive missing polls confirm a real removal. Large enough
    // to ride out transient unavailability, small enough to react to a real
    // delete.
    static constexpr int kOfflineThreshold = 2;

    // Reads a directory entry's file identity (size + last-write time).
    // Returns a zeroed stamp on filesystem error.
    static FileStamp readStamp(const std::filesystem::directory_entry& entry);
    std::vector<std::filesystem::path> snapshot_;
    mutable std::mutex mutex_;
};

} // namespace lrc