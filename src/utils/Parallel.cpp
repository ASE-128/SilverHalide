#include "utils/Parallel.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
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

        // Shared completion state.
        //
        // This MUST live on the heap rather than in this stack frame. The last
        // task's guard decrements `remaining` and then wakes the waiter, so the
        // caller can observe a zero count, return, and destroy its frame while
        // that same task is still inside notify_all() -- a use-after-free on the
        // mutex/condition_variable that shows up intermittently as a crash
        // inside Cnd_broadcast, as heap corruption, or as a hang. Owning the
        // state through a shared_ptr keeps it alive until the late notifier is
        // finished with it.
        struct ForState {
            std::atomic<size_t> next{0};
            std::atomic<size_t> remaining{0};
            std::mutex doneMutex;
            std::condition_variable doneCondition;
        };
        // Counts down from the number of submitted tasks; the caller must not
        // return until every enqueued task has actually run (a task left in the
        // queue would still hold references to this stack frame).
        const size_t taskCount = workers_.size();
        auto st = std::make_shared<ForState>();
        st->remaining.store(taskCount);

        {
            std::unique_lock<std::mutex> lock(mutex_);
            size_t enqueued = 0;
            try {
                for (; enqueued < taskCount; ++enqueued) {
                    // `n`, `g` and `fn` stay by reference: every task has left
                    // them before `remaining` can reach zero. The shared state
                    // itself is captured by value so it outlives this frame.
                    tasks_.emplace_back([st, &n, &g, &fn] {
                        // RAII so `remaining` is released even if fn throws.
                        struct Guard {
                            std::shared_ptr<ForState> st;
                            ~Guard() {
                                if (st->remaining.fetch_sub(1) == 1) {
                                    std::lock_guard<std::mutex> lock(
                                        st->doneMutex);
                                    st->doneCondition.notify_all();
                                }
                            }
                        } guard{st};
                        for (;;) {
                            const size_t b = st->next.fetch_add(g);
                            if (b >= n)
                                break;
                            fn(b, std::min(b + g, n));
                        }
                    });
                }
            } catch (...) {
                // Enqueueing a task threw (e.g. bad_alloc). The tasks already
                // pushed still hold references to this stack frame, so before
                // this frame unwinds we must wait until every one of them has
                // run, or a worker would execute fn on a dead stack frame. Fix
                // `remaining` to the number actually enqueued (workers cannot
                // pop while we hold the pool lock), release the pool lock so
                // the workers can drain them, then rethrow. Tasks belonging to
                // other concurrent parallelFor calls are left untouched.
                st->remaining.store(enqueued);
                lock.unlock();
                std::unique_lock<std::mutex> dl(st->doneMutex);
                st->doneCondition.wait(dl,
                                       [&] { return st->remaining.load() == 0; });
                throw;
            }
        }
        condition_.notify_all();

        // The calling thread joins in on the remaining chunks.
        for (;;) {
            const size_t b = st->next.fetch_add(g);
            if (b >= n)
                break;
            try {
                fn(b, std::min(b + g, n));
            } catch (...) {
                // fn threw, but worker tasks may still be running fn. Before
                // unwinding this frame we must wait until every task has
                // finished (each task's Guard releases `remaining`), or a
                // worker would keep executing fn on a dead stack frame
                // (use-after-free). Then rethrow the original exception
                // unchanged.
                std::unique_lock<std::mutex> lock(st->doneMutex);
                st->doneCondition.wait(lock,
                                       [&] { return st->remaining.load() == 0; });
                throw;
            }
        }

        std::unique_lock<std::mutex> lock(st->doneMutex);
        st->doneCondition.wait(lock, [&] { return st->remaining.load() == 0; });
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
                if (stop_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            try {
                task();
            } catch (...) {
                // A chunk task may throw; its Guard has already released
                // `remaining`, so the waiting parallelFor caller can wake up.
                // Swallow it here: letting the exception escape a std::thread
                // body would call std::terminate. Unprocessed chunks are fn's
                // responsibility, not the pool's; the worker keeps looping.
            }
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
