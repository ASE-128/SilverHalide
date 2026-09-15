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

// Number of worker threads (>= 1). Controlled by the LRC_THREADS environment
// variable when set, otherwise std::thread::hardware_concurrency(). Starting
// the pool is the side effect of the first call; it is instant in practice.
unsigned threadCount();

// A reasonable default chunk size for a buffer of `n` elements: targets ~8
// chunks per worker so scheduling stays cheap while still balancing load.
size_t defaultGrain(size_t n);

// Runs fn(begin, end) for a set of chunks that exactly cover [0, n). Each
// chunk is a contiguous half-open range. `grain` is the chunk size in
// elements; when n <= grain (or the pool has a single worker, or the caller
// is itself a pool worker) the whole range runs inline.
void parallelFor(size_t n, size_t grain,
                 const std::function<void(size_t, size_t)>& fn);

} // namespace lrc::parallel
