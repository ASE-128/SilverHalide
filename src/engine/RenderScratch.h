#pragma once

// Thread-local scratch-buffer pool for the engine render hot path.
//
// The Gaussian-blur / luma passes used to heap-allocate one or more w*h
// std::vector<float> per call. This header provides a thread-local pool that
// hands out reusable buffers, so the steady-state render path performs no
// allocation. Buffers are returned to the pool on scope exit through the RAII
// ScratchBuffer guard; the raw scratchVec / scratchRelease pair is also exposed
// for the cases that need a plain vector reference.
//
// Pure C++20, Qt-free, header-only (no .cpp, no CMake registration). Shutdown
// contract: each thread's pool is owned by that thread and destroyed with
// it at thread exit; call lrc::scratchPoolDrain() to release the calling
// thread's buffers deterministically (e.g. before a long-lived worker exits).
// A ScratchBuffer must never outlive the thread that acquired it.

#include <cstddef>
#include <deque>
#include <vector>

namespace lrc {

namespace detail {

struct ScratchPool {
    // deque (not vector): references/pointers to existing elements stay valid
    // when the pool grows, so a ScratchBuffer held while another buffer is
    // acquired never dangles.
    std::deque<std::vector<float>> bufs;
    std::vector<unsigned char> inUse;  // 1 = handed out, 0 = available
};

inline ScratchPool& scratchPool() {
    static thread_local ScratchPool pool;
    return pool;
}

/**
 * @brief Releases every pooled scratch buffer for the calling thread.
 *
 * Idempotent; the pool re-populates on demand after a drain. Call this from a
 * long-lived worker thread before it exits to release the pooled buffers
 * deterministically instead of at an unpredictable point during process
 * teardown.
 *
 * @note Only the calling thread's own pool is touched; cross-thread pools are
 *       released by their own thread-exit destructors.
 */
inline void scratchPoolDrain() {
    ScratchPool& p = scratchPool();
    p.bufs.clear();
    p.inUse.clear();
}

// Grows `v` to hold minElems, or shrinks it when the requested size is a clear
// size-class drop below the current capacity. The shrink bounds memory after a
// one-off full-resolution render; steady-state calls (same working size) reuse
// without reallocating.
inline void scratchResize(std::vector<float>& v, size_t minElems) {
    if (v.size() >= minElems) {
        if (v.capacity() > minElems * 4u + 1024u) {
            std::vector<float> fresh(minElems);
            v.swap(fresh);
        }
        return;
    }
    v.resize(minElems);
}

}  // namespace detail

/**
 * @brief Acquires a thread-local float buffer of at least minElems elements.
 *
 * The buffer is marked in use. Hand it back with `scratchRelease` (or use the
 * `ScratchBuffer` RAII guard, which does so on scope exit). Buffers are reused
 * across calls and retained for the life of the thread; callers must not access
 * a buffer after release.
 *
 * @param minElems Minimum float capacity requested.
 * @return Reference to the acquired buffer.
 */
inline std::vector<float>& scratchVec(size_t minElems) {
    lrc::detail::ScratchPool& p = lrc::detail::scratchPool();
    for (size_t i = 0; i < p.bufs.size(); ++i) {
        if (!p.inUse[i]) {
            p.inUse[i] = 1;
            lrc::detail::scratchResize(p.bufs[i], minElems);
            return p.bufs[i];
        }
    }
    p.inUse.push_back(1);
    p.bufs.emplace_back();
    p.bufs.back().resize(minElems);
    return p.bufs.back();
}

/**
 * @brief Returns a scratch buffer acquired by `scratchVec` back to the pool.
 *
 * Passing a buffer that was not acquired via `scratchVec` (for that thread) is
 * a no-op.
 */
inline void scratchRelease(std::vector<float>& v) {
    lrc::detail::ScratchPool& p = lrc::detail::scratchPool();
    for (size_t i = 0; i < p.bufs.size(); ++i) {
        if (&p.bufs[i] == &v) {
            p.inUse[i] = 0;
            return;
        }
    }
}

/**
 * @brief RAII guard around a scratch buffer.
 *
 * Acquires on construction, releases on destruction, so a forgotten release can
 * never leak a slot or grow the pool. Non-copyable; movable transfers ownership
 * of the wrapped buffer.
 */
class ScratchBuffer {
public:
    ScratchBuffer() : vec_(nullptr) {}
    explicit ScratchBuffer(size_t minElems) : vec_(&lrc::scratchVec(minElems)) {}
    ScratchBuffer(const ScratchBuffer&) = delete;
    ScratchBuffer& operator=(const ScratchBuffer&) = delete;
    ScratchBuffer(ScratchBuffer&& o) noexcept : vec_(o.vec_) { o.vec_ = nullptr; }
    ScratchBuffer& operator=(ScratchBuffer&& o) noexcept {
        if (this != &o) {
            release();
            vec_ = o.vec_;
            o.vec_ = nullptr;
        }
        return *this;
    }
    ~ScratchBuffer() { release(); }

    float* data() { return vec_ ? vec_->data() : nullptr; }
    const float* data() const { return vec_ ? vec_->data() : nullptr; }
    size_t size() const { return vec_ ? vec_->size() : 0; }
    std::vector<float>& vec() { return *vec_; }
    const std::vector<float>& vec() const { return *vec_; }
    float& operator[](size_t i) { return (*vec_)[i]; }
    const float& operator[](size_t i) const { return (*vec_)[i]; }
    explicit operator bool() const { return vec_ != nullptr; }

private:
    void release() {
        if (vec_) {
            lrc::scratchRelease(*vec_);
            vec_ = nullptr;
        }
    }
    std::vector<float>* vec_;
};

}  // namespace lrc
