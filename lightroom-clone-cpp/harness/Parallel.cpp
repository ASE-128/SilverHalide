#include "utils/Parallel.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace lrc::parallel {

namespace {

// True on pool worker threads. Used to detect a nested parallelFor so a
// worker never submits work it would have to wait for itself (deadlock).
thread_local bool tInPool = false;

class Pool {
public:
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    static Pool& instance() {
        static Pool pool;
        return pool;
    }

    ~Pool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        for (std::thread& w : workers_)
            if (w.joinable())
                w.join();
    }

    size_t workerCount() const { return workers_.size(); }

    void parallelFor(size_t n, size_t grain,
                     const std::function<void(size_t, size_t)>& fn) {
        if (n == 0)
            return;
        const size_t g = std::max<size_t>(1, grain);
        // A pool worker calling parallelFor must not enqueue work it would
        // then wait for itself (that deadlocks the pool); run it inline.
        if (tInPool || workers_.empty() || n <= g) {
            fn(0, n);
            return;
        }

        std::atomic<size_t> next{0};
        // Counts down from the number of submitted tasks; the caller must not
        // return until every enqueued task has actually run (a task left in the
        // queue would still hold references to this stack frame).
        const size_t taskCount = workers_.size();
        std::atomic<size_t> remaining{taskCount};
        std::mutex doneMutex;
        std::condition_variable doneCondition;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (size_t w = 0; w < taskCount; ++w) {
                tasks_.emplace_back([&] {
                    // RAII so `remaining` is released even if fn throws.
                    struct Guard {
                        std::atomic<size_t>& counter;
                        std::mutex& m;
                        std::condition_variable& cv;
                        ~Guard() {
                            // Lock the completion mutex BEFORE decrementing the
                            // counter. The caller's wait predicate observes
                            // counter==0 only while holding this same mutex, so
                            // it cannot return (and destroy these stack
                            // objects) while this worker is still inside the
                            // guard destructor. Without this, the last worker's
                            // fetch_sub can expose counter==0 before it has
                            // locked/notified, letting the caller return early
                            // and use-after-free doneMutex/doneCondition.
                            std::lock_guard<std::mutex> lock(m);
                            if (counter.fetch_sub(1) == 1) {
                                cv.notify_all();
                            }
                        }
                    } guard{remaining, doneMutex, doneCondition};
                    for (;;) {
                        const size_t b = next.fetch_add(g);
                        if (b >= n)
                            break;
                        fn(b, std::min(b + g, n));
                    }
                });
            }
        }
        condition_.notify_all();

        // The calling thread joins in on the remaining chunks.
        for (;;) {
            const size_t b = next.fetch_add(g);
            if (b >= n)
                break;
            fn(b, std::min(b + g, n));
        }

        std::unique_lock<std::mutex> lock(doneMutex);
        doneCondition.wait(lock, [&] { return remaining.load() == 0; });
    }

private:
    Pool() {
        unsigned count = 1;
        if (const char* env = std::getenv("LRC_THREADS")) {
            const int v = std::atoi(env);
            if (v >= 1)
                count = static_cast<unsigned>(v);
        } else {
            const unsigned hw = std::thread::hardware_concurrency();
            count = hw > 0 ? hw : 1;
        }
        workers_.reserve(count);
        for (unsigned i = 0; i < count; ++i)
            workers_.emplace_back(&Pool::workerMain, this);
    }

    void workerMain() {
        tInPool = true;
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [&] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty())
                    return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stop_ = false;
};

} // namespace

unsigned threadCount() {
    return static_cast<unsigned>(Pool::instance().workerCount());
}

size_t defaultGrain(size_t n) {
    const size_t threads = std::max<size_t>(1, threadCount());
    // Targets ~8 chunks per worker. The lower bound is small so row-wise
    // splits (h ~ 1000) still produce enough chunks to feed every worker.
    return std::max<size_t>(64, n / (threads * 8));
}

void parallelFor(size_t n, size_t grain,
                 const std::function<void(size_t, size_t)>& fn) {
    Pool::instance().parallelFor(n, grain, fn);
}

} // namespace lrc::parallel
