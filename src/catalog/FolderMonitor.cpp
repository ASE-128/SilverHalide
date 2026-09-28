#include "catalog/FolderMonitor.h"

#include <algorithm>
#include <cctype>
#include <utility>

// Win32 watcher path is Windows-only (the app is Windows-only anyway).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "engine/ImageFormats.h"

namespace lrc {

namespace {
// Watch buffer size: 64 KiB is the recommended minimum that makes buffer
// overflow (ERROR_NOTIFY_ENUM_DIR) rare even under bursts of changes.
constexpr DWORD kWatchBufferBytes = 64 * 1024;
// Change filter: file name changes (create/delete/rename) plus content writes
// (last-write time/size). Non-recursive — poll() scans top level only.
constexpr DWORD kWatchFilter = FILE_NOTIFY_CHANGE_FILE_NAME |
                               FILE_NOTIFY_CHANGE_LAST_WRITE |
                               FILE_NOTIFY_CHANGE_SIZE;
} // namespace

// All Win32 state of the event watcher. Owned by startEventDriven();
// constructed on the calling thread, used exclusively by the watcher thread
// while it runs, destroyed after the thread is joined.
struct FolderMonitor::EventDrivenState {
    HANDLE directory = INVALID_HANDLE_VALUE;
    HANDLE stopEvent = nullptr;   // manual-reset, set by stopEventDriven()
    OVERLAPPED overlapped{};      // hEvent created by the watcher thread
    std::vector<char> buffer;

    ~EventDrivenState() {
        if (directory != INVALID_HANDLE_VALUE)
            CloseHandle(directory);
        if (stopEvent)
            CloseHandle(stopEvent);
    }
};

FolderMonitor::~FolderMonitor() {
    stopEventDriven();
}

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
    // A poll() rescans the whole folder, so any wake notification pending
    // from the event watcher is consumed here (see requestRecalc()).
    recalcPending_.store(false, std::memory_order_release);
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

bool FolderMonitor::startEventDriven() {
    std::lock_guard<std::mutex> lock(eventMutex_);
    if (eventThread_.joinable())
        return eventDrivenActive_.load(std::memory_order_acquire);

    // Opening the directory up front (with FILE_LIST_DIRECTORY access) doubles
    // as a capability probe: when the folder is missing or sits on an
    // unsupported volume, the handle fails and the caller falls back to the
    // polling path.
    auto state = std::make_unique<EventDrivenState>();
    state->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!state->stopEvent)
        return false;
    state->directory = CreateFileW(
        folder_.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (state->directory == INVALID_HANDLE_VALUE)
        return false;  // EventDrivenState dtor closes stopEvent.
    state->buffer.resize(kWatchBufferBytes);
    state->overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!state->overlapped.hEvent)
        return false;

    // Issue the first ReadDirectoryChangesW *synchronously* so the watch is
    // already pending when startEventDriven() returns. Otherwise a file change
    // landing between thread spawn and the first watch would be missed:
    // ReadDirectoryChangesW only reports changes that occur after the read is
    // queued. The watcher thread then takes over the wait/re-arm loop.
    DWORD bytes = 0;
    if (!ReadDirectoryChangesW(
            state->directory, state->buffer.data(),
            static_cast<DWORD>(state->buffer.size()), FALSE, kWatchFilter,
            &bytes, &state->overlapped, nullptr)) {
        CloseHandle(state->overlapped.hEvent);
        return false;
    }

    stopRequested_.store(false, std::memory_order_relaxed);
    eventState_ = std::move(state);
    eventDrivenActive_.store(true, std::memory_order_release);
    eventThread_ =
        std::thread(&FolderMonitor::eventThreadMain, this, eventState_.get());
    return true;
}

void FolderMonitor::stopEventDriven() {
    std::thread t;
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        if (!eventThread_.joinable())
            return;
        stopRequested_.store(true, std::memory_order_release);
        if (eventState_ && eventState_->stopEvent)
            SetEvent(eventState_->stopEvent);
        t = std::move(eventThread_);
    }
    // Join outside the lock: the watcher thread never takes eventMutex_, so
    // this cannot deadlock, and poll()/callback run on other threads freely.
    if (t.joinable())
        t.join();
    std::lock_guard<std::mutex> lock(eventMutex_);
    eventDrivenActive_.store(false, std::memory_order_release);
    eventState_.reset();
}

bool FolderMonitor::eventDrivenActive() const {
    return eventDrivenActive_.load(std::memory_order_acquire);
}

void FolderMonitor::setWakeCallback(std::function<void()> wake) {
    std::lock_guard<std::mutex> lock(wakeMutex_);
    wake_ = std::move(wake);
}

void FolderMonitor::requestRecalc() {
    // Coalescing: one wake per burst of OS events is enough because poll()
    // rescans the entire folder. poll() clears the flag, arming the next wake.
    if (recalcPending_.exchange(true, std::memory_order_acq_rel))
        return;
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        wake = wake_;
    }
    if (wake)
        wake();
}

void FolderMonitor::eventThreadMain(EventDrivenState* st) {
    // The first ReadDirectoryChangesW was queued by startEventDriven(); this
    // loop only waits for completion, notifies, then re-arms the next read.
    HANDLE waits[2] = {st->overlapped.hEvent, st->stopEvent};
    // True when the thread exits for a reason other than a graceful stopEvent.
    // The owner relies on this to resume the polling fallback.
    bool abnormalExit = false;

    while (!stopRequested_.load(std::memory_order_relaxed)) {
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0 + 1) {  // stopEvent -> graceful shutdown
            CancelIoEx(st->directory, &st->overlapped);
            break;
        }
        if (wait != WAIT_OBJECT_0) {
            abnormalExit = true;
            break;
        }
        DWORD bytes = 0;
        if (!GetOverlappedResult(st->directory, &st->overlapped, &bytes,
                                 FALSE)) {
            // Buffer overflow (ERROR_NOTIFY_ENUM_DIR): too many changes for
            // one notification — still worth one wake, poll() rescans anyway.
            if (GetLastError() == ERROR_NOTIFY_ENUM_DIR)
                requestRecalc();
        } else {
            requestRecalc();
        }
        ResetEvent(st->overlapped.hEvent);
        // Re-arm the next read. A failure here (directory unreadable/closed)
        // ends the event thread; wake the owner once so it re-polls and, seeing
        // eventDrivenActive() == false, resumes the polling fallback.
        DWORD armBytes = 0;
        if (!ReadDirectoryChangesW(
                st->directory, st->buffer.data(),
                static_cast<DWORD>(st->buffer.size()), FALSE, kWatchFilter,
                &armBytes, &st->overlapped, nullptr)) {
            abnormalExit = true;
            break;
        }
    }

    if (st->overlapped.hEvent) {
        CloseHandle(st->overlapped.hEvent);
        st->overlapped.hEvent = nullptr;
    }
    // The directory handle stays open until EventDrivenState destruction
    // (after join), keeping this thread's teardown minimal.
    eventDrivenActive_.store(false, std::memory_order_release);
    // On an abnormal exit (not a user stopEvent) the owner's poll callback may
    // no longer be scheduled by OS events, so nudge it once; it will detect the
    // inactive event watcher and fall back to periodic polling.
    if (abnormalExit)
        requestRecalc();
}

} // namespace lrc