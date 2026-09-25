#pragma once

/**
 * @brief Folder watcher (std::filesystem poll + optional Win32 event watcher,
 *        Qt-free).
 *
 * Two change-detection paths share one diff/grace logic:
 *  - Polling (default): the UI drives poll() from a QTimer; a full directory
 *    scan is diffed against the known file set.
 *  - Event-driven (Windows only): a background thread waits on
 *    ReadDirectoryChangesW and, on any change, invokes the wake callback once
 *    so the owner runs a poll() immediately (within seconds) instead of
 *    waiting for the next timer tick. The watcher thread never scans the
 *    directory itself and never calls the change callback — poll() still owns
 *    the diff/grace state machine, so both paths report identical semantics.
 *
 * Shared semantics: beyond path add/remove the monitor also tracks file
 * identity (size + last-write time) so an in-place content change is reported
 * as "modified". Removable files are only reported after they stay gone across
 * kOfflineThreshold consecutive polls; when the whole folder becomes
 * unavailable in a single poll no removal is reported at all, so a transient
 * network/disconnected-drive absence cannot destroy catalog records.
 *
 * Threading: poll()/start()/currentFiles() are serialized by an internal
 * mutex. The event watcher thread only touches its own state plus the wake
 * callback (guarded by a separate mutex); it is stopped by stopEventDriven()
 * or by the destructor, which joins the thread. The change callback always
 * runs on the thread that calls poll() — typically the UI thread — never on
 * the watcher thread.
 */

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

    /// @brief Destroys the monitor, stopping and joining any running event
    ///        watcher thread. Safe to call while polling is active.
    ~FolderMonitor();

    /// @brief Installs the change callback; replaces any previous one.
    void setCallback(ChangeCallback callback);

    /// @brief Records the current file set as the baseline (called on import).
    void start();

    /// @brief Scans the folder and reports differences since the last call.
    void poll();

    /** @brief Starts the Win32 ReadDirectoryChangesW watcher thread.
     *
     *  The watcher thread waits for create/delete/rename/write events in the
     *  watched folder and, on any change, coalesces a single wake notification
     *  via the wake callback (see setWakeCallback). It does not diff or call
     *  the change callback itself: the owner still runs poll() to recompute,
     *  which keeps the event path's added/modified/removed semantics identical
     *  to the polling path.
     *
     *  @return true when the watcher thread is active — the owner should stop
     *          its polling timer; false when the folder cannot be watched (e.g.
     *          missing/removable folder, unsupported filesystem) — the owner
     *          must fall back to the polling path.
     *  @note Only top-level entries are watched (non-recursive), matching the
     *        non-recursive scan in poll().
     */
    bool startEventDriven();

    /// @brief Stops and joins the event watcher thread. Idempotent; the
    ///        destructor calls it automatically. Safe from any thread.
    void stopEventDriven();

    /// @brief Whether the event watcher thread is currently active.
    /// @return True while the watcher thread is running.
    bool eventDrivenActive() const;

    /** @brief Installs the wake callback invoked from the watcher thread.
     *
     *  The callback is called with no arguments each time the watcher thread
     *  coalesces one or more OS change events, signalling "run poll() soon".
     *  It is thread-safe: it runs on the watcher thread, so the owner must
     *  marshal any work back to its own thread (the GUI uses a queued
     *  connection). Passing an empty callback disables wake notifications.
     *
     *  @param wake Wake-up callback, or empty to disable.
     */
    void setWakeCallback(std::function<void()> wake);

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

    // --- Event-driven watcher (Windows only) -------------------------------------
    // All Win32 handles/buffers live in EventDrivenState (defined in the .cpp)
    // so this header stays Windows-free. Lifecycle: created in
    // startEventDriven(), consumed exclusively by the watcher thread while it
    // runs, destroyed after the thread is joined in stopEventDriven()/dtor.
    struct EventDrivenState;
    std::unique_ptr<EventDrivenState> eventState_;
    std::thread eventThread_;
    // Set by stopEventDriven() to ask the watcher thread to exit.
    std::atomic<bool> stopRequested_{false};
    // Coalescing flag: true while a wake notification is still unconsumed by
    // poll(). Lets the watcher thread drop redundant wakes (poll() rescans the
    // whole folder, so one wake covers any burst of changes).
    std::atomic<bool> recalcPending_{false};
    std::atomic<bool> eventDrivenActive_{false};
    // Guards eventState_/eventThread_ and the wake callback. Separate from
    // mutex_ so stopEventDriven() never deadlocks against an in-flight poll().
    mutable std::mutex eventMutex_;
    std::function<void()> wake_;
    mutable std::mutex wakeMutex_;

    // Watcher thread body: waits on ReadDirectoryChangesW, then coalesces a
    // single wake notification via requestRecalc(). Runs on the event thread.
    void eventThreadMain(EventDrivenState* state);
    // Coalesced wake: sets recalcPending_ and, if it was clear, calls wake_.
    // Called from the watcher thread only.
    void requestRecalc();
};

} // namespace lrc