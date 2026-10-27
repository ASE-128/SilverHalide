#pragma once

/**
 * @brief Read-only "Performance diagnostics" window (视图 → 性能诊断…).
 *
 * The dialog is modeless and never touches the render path: it polls the
 * snapshot provider on a 500 ms timer and writes plain label text, so opening
 * it cannot slow down previews. Everything it shows comes from the snapshot;
 * the dialog owns no engine or catalog references.
 */

#include <cstddef>
#include <cstdint>
#include <functional>

#include <QDialog>
#include <QString>

class QLabel;
class QTimer;

namespace lrc {

/**
 * @brief One immutable view of the performance counters.
 *
 * Produced on the UI thread by MainWindow (timing window + Pipeline stats +
 * backend name + process memory). Times are milliseconds; cache byte counts are
 * the preview LRU only (1:1 full-res renders bypass it by design).
 */
struct PerfDiagnosticsSnapshot {
    QString backend;      ///< "d3d11" or "cpu" (actually active backend).
    QString preference;   ///< "auto" or "cpu" (requested preference).
    std::size_t sampleCount = 0;
    double minMs = 0.0;
    double medianMs = 0.0;
    double maxMs = 0.0;
    std::uint64_t cacheHits = 0;
    std::uint64_t cacheMisses = 0;
    double cacheHitRate = 0.0;  ///< hits / (hits + misses), 0 when no lookups.
    std::size_t cacheBytes = 0;
    std::size_t cacheCapacityBytes = 0;
    std::uint64_t workingSetBytes = 0;      ///< 0 when unavailable.
    std::uint64_t peakWorkingSetBytes = 0;  ///< 0 when unavailable.
};

class PerfDiagnosticsDialog : public QDialog {
    Q_OBJECT
public:
    using SnapshotProvider = std::function<PerfDiagnosticsSnapshot()>;

    /**
     * @param provider Called on every refresh; must be cheap (counter reads)
     *                 and must stay valid for the dialog's lifetime.
     */
    explicit PerfDiagnosticsDialog(SnapshotProvider provider,
                                   QWidget* parent = nullptr);

public slots:
    /// Re-reads the snapshot and repaints every label.
    void refreshNow();

private:
    SnapshotProvider provider_;
    QTimer* timer_ = nullptr;
    QLabel* backendLabel_ = nullptr;
    QLabel* timingLabel_ = nullptr;
    QLabel* cacheLabel_ = nullptr;
    QLabel* memoryLabel_ = nullptr;
};

}  // namespace lrc
