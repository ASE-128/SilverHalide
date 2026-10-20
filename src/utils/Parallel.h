#pragma once

// Lightweight CPU thread pool used to parallelise the pixel loops of the
// render pipeline. Qt-free so the engine and catalog layers can use it.
//
// The pool starts lazily on first use with one worker per hardware thread
// (override with the LRC_THREADS environment variable). `parallelFor` splits
// the index range [0, n) into contiguous chunks and runs them on the pool
// while the calling thread participates as well; the call blocks until every
// chunk has run.
//
// Thread-safety contract: chunks of a single call never overlap, so `fn` may
// write to per-chunk output regions without locks. Nested parallelFor calls
// are safe -- when the current thread is itself a pool worker the inner range
// is executed inline, so the pool can never deadlock on its own tasks.
//
// Inline fast path: `parallelFor` is a template so the two degenerate cases
// -- the whole range fits in one chunk, or the caller already runs on a pool
// worker -- invoke `fn` directly on the calling thread without building a
// std::function and without touching the pool. Only calls that really dispatch
// work pay for one type erasure at the pool boundary. The chunk splitting and
// the scheduling order are unchanged by this optimisation.

#include <algorithm>
#include <cstddef>
#include <functional>
#include <utility>

namespace lrc::parallel {

/** @brief Number of worker threads backing the pool (>= 1).
 *
 * Controlled by the LRC_THREADS environment variable when set, otherwise
 * std::thread::hardware_concurrency(). Starting the pool is a side effect of
 * the first call; it is instant in practice.
 */
unsigned threadCount();

/** @brief A reasonable default chunk size for a buffer of `n` elements.
 *
 * Targets ~8 chunks per worker so scheduling stays cheap while still balancing
 * load.
 */
size_t defaultGrain(size_t n);

/** @brief Internal helpers backing the inline fast path. Not for general use.
 *
 * They are declared in the public header only because the `parallelFor`
 * template below needs to call them; treat them as implementation details.
 */
namespace detail {

/** @brief Reports whether the calling thread is one of the pool's workers.
 *
 * Used to keep nested `parallelFor` calls inline: a worker that submitted
 * work and then waited for it would deadlock the pool.
 *
 * @return true when the calling thread was started by the pool, false
 *         otherwise (including threads that merely use it).
 *
 * @note Never blocks and never allocates; safe to call from any thread.
 *       Does not start the pool.
 */
bool onPoolThread() noexcept;

/** @brief Type-erased dispatch used by `parallelFor` for real pool work.
 *
 * Splits [0, n) into contiguous chunks of `grain` elements, enqueues one
 * draining task per worker, has the calling thread take chunks as well, and
 * blocks until every chunk has run. Tasks that run on a worker and a caller
 * that finds the range exhausted simply finish; nothing is ever enqueued from
 * inside a pool worker.
 *
 * @param n     Range length; must be > 0.
 * @param grain Chunk size; values < 1 are clamped to 1.
 * @param fn    Per-chunk callback. It is invoked as fn(begin, end) with
 *              half-open, non-overlapping ranges. The reference must stay
 *              valid until the call returns. Exceptions thrown by fn are
 *              rethrown unchanged on the calling thread after every in-flight
 *              chunk has finished, so no callback outlives this frame.
 *
 * @note This is the only place where a std::function is built; direct callers
 *       are expected only from `parallelFor` (and from tests that pin the
 *       chunking contract).
 */
void parallelForErased(size_t n, size_t grain,
                       const std::function<void(size_t, size_t)>& fn);

} // namespace detail

/** @brief Runs fn(begin, end) for chunks that exactly cover [0, n).
 *
 * Each chunk is a contiguous half-open range of at most `grain` elements;
 * together they cover [0, n) exactly once. When n == 0 nothing is called. When
 * in doubt the chunking is defined by the implementation (chunks are taken
 * from a shared atomic cursor), not by this header.
 *
 * Fast paths: if n <= grain, or the calling thread is already a pool worker,
 * fn(0, n) runs inline on the calling thread -- no std::function is created,
 * no task is enqueued and the pool is not started. Otherwise the work is
 * dispatched through the pool and the calling thread participates until the
 * range is exhausted.
 *
 * @param n     Range length. 0 means "no work" and does not invoke fn.
 * @param grain Maximum element count per chunk; values < 1 are clamped to 1.
 * @param fn    Callable accepting (size_t begin, size_t end). It must not
 *              call `parallelFor` on the same output regions recursively;
 *              nested calls are safe (they run inline) but share the caller's
 *              thread. Chunks never overlap, so fn may write per-chunk output
 *              regions without locks.
 *
 * @note Blocks until every chunk has run. Exceptions thrown by fn are
 *       propagated to the caller after the in-flight chunks finish; an
 *       exception thrown on a pool worker itself is swallowed by the worker
 *       loop (it cannot escape a std::thread body), but the call still waits
 *       for that chunk before returning.
 */
template <class Fn>
void parallelFor(size_t n, size_t grain, Fn&& fn) {
    if (n == 0)
        return;
    const size_t g = std::max<size_t>(1, grain);
    // Zero-cost paths: a single chunk, or a nested call from a pool worker.
    // Keeping these in the header lets the compiler inline fn and avoids the
    // std::function allocation completely.
    if (n <= g || detail::onPoolThread()) {
        fn(0, n);
        return;
    }
    detail::parallelForErased(
        n, g, std::function<void(size_t, size_t)>(std::forward<Fn>(fn)));
}

} // namespace lrc::parallel
