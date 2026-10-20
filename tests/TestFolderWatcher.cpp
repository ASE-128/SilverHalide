// P3-2 — folder monitoring: polling fallback semantics (unchanged) plus the
// Win32 event-driven watcher. The event watcher is Windows-only, so those
// tests are guarded by _WIN32; everything else runs in any headless build.
//
// Semantics under test (shared by both paths, owned by poll()):
//  - added    = importable file present now, unknown before
//  - modified = importable file whose size/last-write identity changed in place
//  - removed  = importable file absent for kOfflineThreshold consecutive polls
//  - a whole-folder scan failure reports nothing (no false removals)
//
// The event-driven tests assert that OS notifications wake the owner within
// seconds (a gate with a bounded wait — no 2s polling timer involved).

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "catalog/FolderMonitor.h"

using namespace lrc;

namespace {

// Unique throwaway directory per test (tests may run in parallel).
std::filesystem::path tempDir() {
    static std::atomic<size_t> counter{0};
    const auto dir =
        std::filesystem::temp_directory_path() /
        ("lrc_folder_watcher_" + std::to_string(++counter) + "_" +
         std::to_string(static_cast<unsigned long long>(
             std::chrono::system_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(dir);
    return dir;
}

void writeFile(const std::filesystem::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
    out.flush();
    out.close();
}

// Bounded wait gate used to observe the event watcher's wake notification.
// The wake runs on the watcher thread; waitFor() is called from the test
// thread, so the gate only needs mutex+condition_variable.
struct WakeGate {
    std::mutex m;
    std::condition_variable cv;
    bool fired = false;

    void notify() {
        {
            std::lock_guard<std::mutex> lock(m);
            fired = true;
        }
        cv.notify_all();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(m);
        fired = false;
    }

    // Returns true when the wake arrived within `timeout`.
    bool waitFor(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, timeout, [this] { return fired; });
    }
};

// Collects change-callback output into stable vectors.
struct ChangeLog {
    std::mutex m;
    std::vector<std::filesystem::path> added;
    std::vector<std::filesystem::path> modified;
    std::vector<std::filesystem::path> removed;

    void record(const std::vector<std::filesystem::path>& a,
                const std::vector<std::filesystem::path>& mod,
                const std::vector<std::filesystem::path>& rem) {
        std::lock_guard<std::mutex> lock(m);
        added.insert(added.end(), a.begin(), a.end());
        modified.insert(modified.end(), mod.begin(), mod.end());
        removed.insert(removed.end(), rem.begin(), rem.end());
    }
};

bool contains(const std::vector<std::filesystem::path>& v,
              const std::filesystem::path& p) {
    return std::find(v.begin(), v.end(), p) != v.end();
}

// Aligns the test's one-shot gate with the watcher's coalescing semantics:
// the watcher merges any change arriving while a wake is still unconsumed by
// poll(). A file write can produce several OS notifications (create + size +
// last-write), the later ones arriving late and arming the merge flag before
// the next step runs. Draining before each operation (sleep until late events
// land, then poll() to consume them, then reset the gate) makes the subsequent
// wake deterministic.
void drain(WakeGate& gate, FolderMonitor& mon) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    mon.poll();
    gate.reset();
}

} // namespace

TEST_CASE("FolderMonitor poll: baseline then added files", "[catalog][p3-2]") {
    const auto dir = tempDir();
    FolderMonitor mon(dir);
    ChangeLog log;
    mon.setCallback([&](const auto& a, const auto& m, const auto& r) {
        log.record(a, m, r);
    });
    mon.start();  // empty baseline

    const auto jpg = dir / "shot.jpg";
    writeFile(jpg, "jpeg-bytes");
    mon.poll();
    REQUIRE(contains(log.added, jpg));
    REQUIRE(log.removed.empty());

    // Non-importable files are filtered out.
    const auto txt = dir / "notes.txt";
    writeFile(txt, "ignored");
    mon.poll();
    REQUIRE(!contains(log.added, txt));
}

TEST_CASE("FolderMonitor poll: in-place modification reported once",
          "[catalog][p3-2]") {
    const auto dir = tempDir();
    FolderMonitor mon(dir);
    ChangeLog log;
    mon.setCallback([&](const auto& a, const auto& m, const auto& r) {
        log.record(a, m, r);
    });
    const auto p = dir / "img.png";
    writeFile(p, "aaa");
    mon.start();

    writeFile(p, "longer-content-bbb");  // size changes -> modified
    mon.poll();
    REQUIRE(contains(log.modified, p));
    REQUIRE(log.added.empty());
}

TEST_CASE("FolderMonitor poll: removal needs kOfflineThreshold absent polls",
          "[catalog][p3-2]") {
    const auto dir = tempDir();
    FolderMonitor mon(dir);
    ChangeLog log;
    mon.setCallback([&](const auto& a, const auto& m, const auto& r) {
        log.record(a, m, r);
    });
    const auto p = dir / "img.arw";
    writeFile(p, "raw");
    mon.start();

    std::filesystem::remove(p);
    mon.poll();  // 1st missing poll: starts the grace counter, no report
    REQUIRE(log.removed.empty());
    mon.poll();  // 2nd missing poll: threshold reached
    REQUIRE(contains(log.removed, p));
}

TEST_CASE("FolderMonitor poll: whole-folder unavailability reports nothing",
          "[catalog][p3-2]") {
    const auto dir = tempDir();
    const auto hidden = dir.parent_path() /
                        (dir.filename().string() + "_moved");
    FolderMonitor mon(dir);
    ChangeLog log;
    mon.setCallback([&](const auto& a, const auto& m, const auto& r) {
        log.record(a, m, r);
    });
    const auto p = dir / "img.tiff";
    writeFile(p, "tiff");
    mon.start();

    // The folder disappears in one poll: transient absence must not be
    // mistaken for deletion of every file.
    std::filesystem::rename(dir, hidden);
    mon.poll();
    REQUIRE(log.removed.empty());
    REQUIRE(log.added.empty());

    std::filesystem::rename(hidden, dir);
    mon.poll();
    REQUIRE(log.removed.empty());
}

#ifdef _WIN32

TEST_CASE("FolderMonitor event-driven: create/modify/remove wake within "
          "seconds (no 2s poll)",
          "[catalog][p3-2][windows]") {
    const auto dir = tempDir();
    FolderMonitor mon(dir);
    ChangeLog log;
    WakeGate gate;
    mon.setCallback([&](const auto& a, const auto& m, const auto& r) {
        log.record(a, m, r);
    });
    mon.start();  // empty baseline

    if (!mon.startEventDriven()) {
        SKIP("ReadDirectoryChangesW unavailable on this machine");
    }
    REQUIRE(mon.eventDrivenActive());
    mon.setWakeCallback([&] { gate.notify(); });

    // Create: OS event -> wake -> one poll() reports the file, well under the
    // old 2s polling period.
    drain(gate, mon);
    const auto jpg = dir / "new.jpg";
    writeFile(jpg, "v1");
    REQUIRE(gate.waitFor(std::chrono::seconds(5)));
    mon.poll();
    REQUIRE(contains(log.added, jpg));

    // Modify in place: size change -> wake -> poll() reports modified.
    drain(gate, mon);
    writeFile(jpg, "v2-longer-content");
    REQUIRE(gate.waitFor(std::chrono::seconds(5)));
    mon.poll();
    REQUIRE(contains(log.modified, jpg));

    // Remove: wake -> first poll starts the grace counter, second reports.
    drain(gate, mon);
    std::filesystem::remove(jpg);
    REQUIRE(gate.waitFor(std::chrono::seconds(5)));
    mon.poll();
    REQUIRE(log.removed.empty());
    mon.poll();
    REQUIRE(contains(log.removed, jpg));

    mon.stopEventDriven();
    REQUIRE(!mon.eventDrivenActive());
}

TEST_CASE("FolderMonitor event-driven: unwatchable folder falls back to poll",
          "[catalog][p3-2][windows]") {
    const auto missing = tempDir() / "does_not_exist";
    FolderMonitor mon(missing);
    REQUIRE_FALSE(mon.startEventDriven());  // CreateFileW fails -> fallback
    REQUIRE_FALSE(mon.eventDrivenActive());

    // The polling path is still fully usable after the failed start.
    const auto dir = tempDir();
    FolderMonitor mon2(dir);
    ChangeLog log;
    mon2.setCallback([&](const auto& a, const auto& m, const auto& r) {
        log.record(a, m, r);
    });
    mon2.start();
    writeFile(dir / "a.jpg", "x");
    mon2.poll();
    REQUIRE(contains(log.added, dir / "a.jpg"));
}

TEST_CASE("FolderMonitor event-driven: stop is idempotent and safe",
          "[catalog][p3-2][windows]") {
    const auto dir = tempDir();
    FolderMonitor mon(dir);
    REQUIRE(mon.startEventDriven());
    mon.stopEventDriven();
    mon.stopEventDriven();  // second stop is a no-op
    REQUIRE_FALSE(mon.eventDrivenActive());

    // The watcher can be restarted after a stop.
    REQUIRE(mon.startEventDriven());
    REQUIRE(mon.eventDrivenActive());
    mon.stopEventDriven();
}

TEST_CASE("FolderMonitor event-driven: destructor stops and joins the watcher",
          "[catalog][p3-2][windows]") {
    const auto dir = tempDir();
    const auto began = std::chrono::steady_clock::now();
    {
        FolderMonitor mon(dir);
        WakeGate gate;
        mon.setWakeCallback([&] { gate.notify(); });
        REQUIRE(mon.startEventDriven());
        REQUIRE(mon.eventDrivenActive());
        // Leaving this scope destroys the monitor while the watcher thread is
        // still blocked in ReadDirectoryChangesW. The std::jthread destructor
        // requests stop and joins, so this must return instead of hanging.
    }
    const auto elapsed = std::chrono::steady_clock::now() - began;
    REQUIRE(elapsed < std::chrono::seconds(5));
}

#endif  // _WIN32
