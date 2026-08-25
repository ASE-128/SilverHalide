#include "catalog/FolderMonitor.h"

#include <algorithm>
#include <cctype>

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
    snapshot_.clear();
    std::error_code ec;
    if (std::filesystem::exists(folder_, ec) &&
        std::filesystem::is_directory(folder_, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(folder_, ec)) {
            if (entry.is_regular_file(ec) && isImportableFile(entry.path()))
                known_.insert(entry.path().lexically_normal());
        }
    }
    snapshot_.assign(known_.begin(), known_.end());
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

void FolderMonitor::poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::set<std::filesystem::path> current;
    std::error_code ec;
    if (std::filesystem::exists(folder_, ec) &&
        std::filesystem::is_directory(folder_, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(folder_, ec)) {
            if (entry.is_regular_file(ec) && isImportableFile(entry.path()))
                current.insert(entry.path().lexically_normal());
        }
    }

    std::vector<std::filesystem::path> added;
    std::vector<std::filesystem::path> removed;
    for (const auto& p : current)
        if (!known_.count(p))
            added.push_back(p);
    for (const auto& p : known_)
        if (!current.count(p))
            removed.push_back(p);

    known_ = std::move(current);
    snapshot_.assign(known_.begin(), known_.end());

    if (callback_ && (!added.empty() || !removed.empty()))
        callback_(added, removed);
}

const std::vector<std::filesystem::path>& FolderMonitor::currentFiles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

} // namespace lrc
