#include "catalog/FolderMonitor.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "engine/ImageFormats.h"

namespace lrc {

FolderMonitor::FolderMonitor(std::filesystem::path folder)
    : folder_(std::move(folder)) {}

void FolderMonitor::setCallback(ChangeCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

void FolderMonitor::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    known_.clear();
    missingPolls_.clear();
    snapshot_.clear();
    std::error_code ec;
    if (std::filesystem::exists(folder_, ec) &&
        std::filesystem::is_directory(folder_, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(folder_, ec)) {
            if (entry.is_regular_file(ec) && isImportableFile(entry.path()))
                known_[entry.path().lexically_normal()] = readStamp(entry);
        }
    }
    snapshot_.reserve(known_.size());
    for (const auto& [p, st] : known_)
        snapshot_.push_back(p);
}

const std::vector<std::string>& FolderMonitor::rawExtensions() {
    return lrc::rawExtensions();
}

const std::vector<std::string>& FolderMonitor::importExtensions() {
    return lrc::importExtensions();
}

bool FolderMonitor::isRawFile(const std::filesystem::path& path) {
    return lrc::isRawPath(path);
}

bool FolderMonitor::isImportableFile(const std::filesystem::path& path) {
    return lrc::isImportablePath(path);
}

FolderMonitor::FileStamp FolderMonitor::readStamp(
    const std::filesystem::directory_entry& e) {
    FileStamp st;
    std::error_code ec;
    st.size = e.file_size(ec);
    if (ec)
        st.size = 0;
    st.modified = e.last_write_time(ec);
    if (ec)
        st.modified = {};
    return st;
}

void FolderMonitor::poll() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Scan the current file set. The whole scan failing is treated as the
    // folder being transiently unavailable, not as every file being deleted.
    std::map<std::filesystem::path, FileStamp> current;
    bool folderOk = false;
    {
        std::error_code ec;
        if (std::filesystem::exists(folder_, ec) &&
            std::filesystem::is_directory(folder_, ec)) {
            std::error_code ierr;
            std::filesystem::directory_iterator it(folder_, ierr), end;
            for (; it != end; it.increment(ierr)) {
                if (ierr)
                    break;
                const auto& entry = *it;
                std::error_code fec;
                if (!entry.is_regular_file(fec) ||
                    !isImportableFile(entry.path()))
                    continue;
                current[entry.path().lexically_normal()] = readStamp(entry);
            }
            folderOk = true;
        }
    }

    // If the folder could not be enumerated this poll, keep the known set
    // and the offline counters unchanged so a transient absence is never
    // mistaken for deletion. Report nothing (no adds/removes/modifies either).
    if (!folderOk) {
        snapshot_.clear();
        snapshot_.reserve(known_.size());
        for (const auto& [p, st] : known_)
            snapshot_.push_back(p);
        return;
    }

    std::vector<std::filesystem::path> added, modified, removed;

    // New files (present now, unknown before).
    for (const auto& [p, st] : current)
        if (!known_.count(p)) {
            added.push_back(p);
            known_[p] = st;
            missingPolls_.erase(p);
        }

    // Content changes: an existing file whose identity differs was replaced in
    // place. Adopt the new identity and report it.
    for (auto& [p, st] : known_) {
        auto cit = current.find(p);
        if (cit == current.end())
            continue;
        missingPolls_.erase(p);
        if (!(cit->second == st)) {
            modified.push_back(p);
            st = cit->second;
        }
    }

    // Removal grace: only report a file as removed after it has been missing
    // for kOfflineThreshold consecutive polls (the first missing poll just
    // starts the counter). This rides out network/media hiccups while still
    // reacting to a real delete.
    for (auto& [p, n] : missingPolls_) {
        if (current.count(p))
            n = 0;  // re-appeared before the threshold
        else
            ++n;
    }
    for (auto it = known_.begin(); it != known_.end();) {
        if (current.count(it->first)) {
            ++it;
            continue;
        }
        const int n = ++missingPolls_[it->first];
        if (n >= kOfflineThreshold) {
            removed.push_back(it->first);
            missingPolls_.erase(it->first);
            it = known_.erase(it);
        } else {
            ++it;
        }
    }

    snapshot_.clear();
    snapshot_.reserve(known_.size());
    for (const auto& [p, st] : known_)
        snapshot_.push_back(p);

    if (callback_ && (!added.empty() || !modified.empty() || !removed.empty()))
        callback_(added, modified, removed);
}

const std::vector<std::filesystem::path>& FolderMonitor::currentFiles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

} // namespace lrc