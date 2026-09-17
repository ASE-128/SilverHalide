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

#include <cstddef>
#include <functional>

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

/** @brief Runs fn(begin, end) for chunks that exactly cover [0, n).
 *
 * Each chunk is a contiguous half-open range. When n <= grain, the pool has a
 * single worker, or the caller is itself a pool worker, the whole range runs
 * inline.
 *
 * @param n     Range length.
 * @param grain Element count per chunk.
 * @param fn    Called once per chunk; chunks never overlap, so `fn` may write
 *              per-chunk output regions without locks. The call blocks until
 *              every chunk has run.
 */
void parallelFor(size_t n, size_t grain,
                 const std::function<void(size_t, size_t)>& fn);

} // namespace lrc::parallel
