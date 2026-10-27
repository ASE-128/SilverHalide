#pragma once

// Qt-free, fixed-window timing accumulator used by the performance diagnostics.
//
// The preview worker reports one wall-clock sample per refined frame; the UI
// keeps the most recent kDefaultWindow samples and shows min / median / max.
// Median is what AC-6 compares against, so the window and the definition below
// are part of the observable contract: the ring holds at most N samples, the
// median of an even sample count is the mean of the two middle values, and an
// empty window reports 0 for every statistic.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace lrc {

/**
 * @brief Ring buffer of the most recent N timing samples (milliseconds).
 *
 * @note Not thread-safe by design: callers keep one instance on a single
 *       thread (the UI thread). add()/clear() are O(1); the statistic queries
 *       are O(N log N) (median sorts a copy) and are only called at UI refresh
 *       cadence (>= 200 ms).
 */
class PerfStats {
public:
    /// Samples retained by the default window (matches the A2 acceptance rule).
    static constexpr std::size_t kDefaultWindow = 30;

    /**
     * @param window Number of samples to retain; 0 falls back to
     *               kDefaultWindow.
     */
    explicit PerfStats(std::size_t window = kDefaultWindow)
        : window_(window > 0 ? window : kDefaultWindow),
          ring_(window > 0 ? window : kDefaultWindow, 0.0) {}

    /**
     * @brief Appends one sample, evicting the oldest when the window is full.
     * @param ms Wall-clock duration in milliseconds.
     *
     * @note Negative or non-finite samples are ignored: they cannot be a real
     *       duration and would corrupt the distribution.
     */
    void add(double ms) noexcept {
        if (!std::isfinite(ms) || ms < 0.0)
            return;
        ring_[next_] = ms;
        next_ = (next_ + 1) % window_;
        if (count_ < window_)
            ++count_;
    }

    /// Drops every retained sample.
    void clear() noexcept {
        next_ = 0;
        count_ = 0;
    }

    /// @return Number of valid samples currently retained (<= windowSize()).
    std::size_t sampleCount() const noexcept { return count_; }

    /// @return Maximum number of samples the window can hold.
    std::size_t windowSize() const noexcept { return window_; }

    /// @return Median of the retained samples, or 0 when there are none.
    double medianMs() const {
        if (count_ == 0)
            return 0.0;
        std::vector<double> sorted = retained();
        std::sort(sorted.begin(), sorted.end());
        const std::size_t mid = sorted.size() / 2;
        if (sorted.size() % 2 == 0)
            return 0.5 * (sorted[mid - 1] + sorted[mid]);
        return sorted[mid];
    }

    /// @return Smallest retained sample, or 0 when there are none.
    double minMs() const noexcept {
        if (count_ == 0)
            return 0.0;
        double lo = ring_[0];
        for (std::size_t i = 1; i < count_; ++i)
            lo = std::min(lo, ring_[i]);
        return lo;
    }

    /// @return Largest retained sample, or 0 when there are none.
    double maxMs() const noexcept {
        if (count_ == 0)
            return 0.0;
        double hi = ring_[0];
        for (std::size_t i = 1; i < count_; ++i)
            hi = std::max(hi, ring_[i]);
        return hi;
    }

private:
    /// @return The valid samples in arrival order (for sorting / scanning).
    std::vector<double> retained() const {
        std::vector<double> out;
        out.reserve(count_);
        for (std::size_t i = 0; i < count_; ++i)
            out.push_back(ring_[i]);
        return out;
    }

    std::size_t window_;
    std::vector<double> ring_;  // storage for window_ samples
    std::size_t next_ = 0;      // write cursor
    std::size_t count_ = 0;     // valid samples (saturates at window_)
};

}  // namespace lrc
