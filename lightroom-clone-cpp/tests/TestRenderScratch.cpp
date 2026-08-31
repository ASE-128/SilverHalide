// S11: RenderScratch thread-local buffer-pool tests.
//
// RenderScratch.h is a pure-header, Qt-free facility that hands out reusable
// std::vector<float> scratch buffers from a thread_local pool. This file pins
// down the behavioural contracts (independence across threads, RAII move
// semantics, capacity shrink after a one-off oversized request) so future
// refactors of the pool internals (S12 caller-side adoption) never regress
// them.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "engine/RenderScratch.h"

using namespace lrc;
using Catch::Approx;

namespace {

// Returns the pool's current in-use count by walking the inUse flags.
// Deliberately peers into the detail namespace so the test has a precise view
// of pool bookkeeping (test-only, no production code does this).
size_t poolInUseCount() {
    const auto& p = detail::scratchPool();
    size_t n = 0;
    for (unsigned char f : p.inUse)
        if (f) ++n;
    return n;
}

size_t poolCapacityEntries() {
    return detail::scratchPool().bufs.size();
}

}  // namespace

TEST_CASE("RenderScratch: acquire and release recovers pool slot",
          "[RenderScratch][scratch_pool]") {
    // Drain any leftover in-use slots from previous sections that threw or
    // leaked (should be zero after a clean run; defensive).
    // Not strictly required here because the section below is self-contained.
    const size_t entriesBefore = poolCapacityEntries();
    const size_t inUseBefore = poolInUseCount();

    {
        ScratchBuffer sb(256);
        REQUIRE(sb);
        REQUIRE(sb.size() >= 256);
        // Writing into the buffer is permitted and the memory is live.
        std::fill_n(sb.data(), 256, 3.14f);
        REQUIRE(sb[0] == Approx(3.14f));
        REQUIRE(poolInUseCount() == inUseBefore + 1);
    }  // RAII release

    REQUIRE(poolInUseCount() == inUseBefore);
    // The pool keeps the vector around (capacity, not shrunk for a small buf).
    REQUIRE(poolCapacityEntries() >= entriesBefore);
}

TEST_CASE("RenderScratch: thread_local pools are independent across threads",
          "[RenderScratch][scratch_pool][threading]") {
    // Two threads each acquire their own scratch buffer and record the pointer
    // they received plus the pool's capacity entry count visible from their
    // own thread. The pointers must differ (separate pools) and each thread
    // must observe exactly 1 in-use slot while its guard lives.
    std::atomic<bool> go{false};
    float* t1Ptr = nullptr;
    float* t2Ptr = nullptr;
    size_t t1InUse = 0;
    size_t t2InUse = 0;

    auto worker = [&](float*& outPtr, size_t& outInUse) {
        while (!go.load()) std::this_thread::yield();
        ScratchBuffer sb(1024);
        outPtr = sb.data();
        outInUse = poolInUseCount();
        // Keep sb alive long enough for both workers to hold their buffer.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        (void)sb[0];  // touch so the compiler won't optimise sb away.
    };

    std::thread a(worker, std::ref(t1Ptr), std::ref(t1InUse));
    std::thread b(worker, std::ref(t2Ptr), std::ref(t2InUse));
    go.store(true);
    a.join();
    b.join();

    REQUIRE(t1Ptr != nullptr);
    REQUIRE(t2Ptr != nullptr);
    // Separate pools -> separate backing memory.
    REQUIRE(t1Ptr != t2Ptr);
    // Each thread observed exactly one in-use slot (its own acquisition).
    REQUIRE(t1InUse == 1u);
    REQUIRE(t2InUse == 1u);
    // After join, both pools' inUse counts are back to zero on the next
    // occasion we ask them. Probe by re-acquiring once on the main thread
    // after the child threads have exited: the main pool must still report
    // only 1 in-use (it never saw the children's pools).
    {
        ScratchBuffer main(16);
        REQUIRE(poolInUseCount() == 1u);
    }
}

TEST_CASE("RenderScratch: RAII move semantics do not double-release",
          "[RenderScratch][scratch_pool][raii]") {
    const size_t inUse0 = poolInUseCount();

    float* innerPtr = nullptr;
    {
        ScratchBuffer outer;
        {
            ScratchBuffer a(128);
            innerPtr = a.data();
            REQUIRE(poolInUseCount() == inUse0 + 1);
            // Move-construct `outer` from `a`: `a` gives up ownership.
            outer = std::move(a);
            // a now holds nullptr, its destructor must be a no-op.
            REQUIRE(static_cast<bool>(a) == false);
            // outer still points at the same buffer.
            REQUIRE(outer.data() == innerPtr);
            REQUIRE(poolInUseCount() == inUse0 + 1);  // still exactly one slot
        }  // a destroyed (nullptr) -> inUse count must not dip below 1
        REQUIRE(poolInUseCount() == inUse0 + 1);
        REQUIRE(outer.data() == innerPtr);
    }  // outer destroyed -> slot is released.
    REQUIRE(poolInUseCount() == inUse0);

    // Now acquire the same size again. The pool should hand out the same
    // vector (capacity reuse) instead of growing a second entry.
    {
        ScratchBuffer b(128);
        // The pointer is allowed to be the same slot (reuse) or a new one, but
        // the pool's inUse count is what really pins down the bookkeeping.
        REQUIRE(poolInUseCount() == inUse0 + 1);
    }
    REQUIRE(poolInUseCount() == inUse0);
}

TEST_CASE("RenderScratch: one-off oversized buffer shrinks on next small reuse",
          "[RenderScratch][scratch_pool][capacity]") {
    // The scratchResize policy shrinks a buffer when its capacity exceeds
    // (requested size * 4 + 1024). So a 4-Mega request followed by a 16-float
    // request on the same slot must shrink.
    constexpr size_t huge = 4u * 1024u * 1024u;  // 4 Mfloats ≈ 16 MiB
    constexpr size_t tiny = 16u;
    const size_t entries0 = poolCapacityEntries();

    size_t hugeCapacity = 0;
    size_t tinyCapacity = 0;
    {
        ScratchBuffer sb(huge);
        REQUIRE(sb.size() >= huge);
        hugeCapacity = sb.vec().capacity();
    }
    {
        // Re-acquire with a much smaller request on the same thread -> the
        // reusable slot's vector gets shrunk by scratchResize.
        ScratchBuffer sb(tiny);
        REQUIRE(sb.size() >= tiny);
        tinyCapacity = sb.vec().capacity();
    }
    // Shrink must have actually reduced the capacity; otherwise the pool
    // would permanently hog 16 MiB per thread after every full-res render.
    const bool shrank = (tinyCapacity * 2u) < hugeCapacity;
    REQUIRE(shrank);
    // And we must not have allocated a second buffer entry (reuse happened).
    const size_t entriesAfter = poolCapacityEntries();
    const bool reused = (entriesAfter <= entries0 + 1);
    REQUIRE(reused);
}

TEST_CASE("RenderScratch: raw scratchVec / scratchRelease pair behaves like RAII",
          "[RenderScratch][scratch_pool][raw]") {
    const size_t inUse0 = poolInUseCount();
    std::vector<float>& v = scratchVec(64);
    REQUIRE(v.size() >= 64);
    REQUIRE(poolInUseCount() == inUse0 + 1);
    std::fill_n(v.data(), 64, 1.5f);
    scratchRelease(v);
    REQUIRE(poolInUseCount() == inUse0);

    // The handed-back vector must be reused by the next scratchVec(64) on
    // this thread: pool entry count doesn't grow.
    const size_t entries0 = poolCapacityEntries();
    std::vector<float>& v2 = scratchVec(64);
    REQUIRE(poolCapacityEntries() == entries0);
    scratchRelease(v2);
    REQUIRE(poolInUseCount() == inUse0);
}

TEST_CASE("RenderScratch: nested ScratchBuffer acquisitions hold independent slots",
          "[RenderScratch][scratch_pool][nested]") {
    // A gaussianBlur caller may hold one scratch buffer and then call a helper
    // that needs its own. Two nested guards must occupy different slots and
    // release in the right order without corrupting the inUse bookkeeping.
    const size_t inUse0 = poolInUseCount();

    {
        ScratchBuffer outer(32);
        REQUIRE(poolInUseCount() == inUse0 + 1);
        float* outerPtr = outer.data();

        {
            ScratchBuffer inner(32);
            REQUIRE(poolInUseCount() == inUse0 + 2);
            float* innerPtr = inner.data();
            REQUIRE(innerPtr != outerPtr);  // distinct vectors
            std::fill_n(outerPtr, 32, 1.0f);
            std::fill_n(innerPtr, 32, 2.0f);
            REQUIRE(outer[16] == Approx(1.0f));
            REQUIRE(inner[16] == Approx(2.0f));
        }  // inner released

        REQUIRE(poolInUseCount() == inUse0 + 1);
        REQUIRE(outer[16] == Approx(1.0f));  // outer untouched
    }  // outer released

    REQUIRE(poolInUseCount() == inUse0);
}
