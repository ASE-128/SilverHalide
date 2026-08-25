#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include "utils/Parallel.h"

using lrc::parallel::parallelFor;
using lrc::parallel::threadCount;

TEST_CASE("Parallel: pool has at least one worker", "[parallel]") {
    REQUIRE(threadCount() >= 1);
}

TEST_CASE("Parallel: parallelFor covers every element exactly once",
          "[parallel]") {
    const size_t n = 100000;
    std::vector<int> out(n, 0);
    parallelFor(n, 4096, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i)
            out[i] = 1;
    });
    for (size_t i = 0; i < n; ++i)
        REQUIRE(out[i] == 1);
}

TEST_CASE("Parallel: chunks are contiguous, non-overlapping and gap-free",
          "[parallel]") {
    // grain chosen to not divide n, exercising the tail chunk.
    const size_t n = 50000;
    const size_t grain = 333;
    std::vector<char> covered(n, 0);
    std::atomic<size_t> total{0};
    std::atomic<size_t> overlaps{0};
    std::atomic<size_t> invalidRanges{0};
    parallelFor(n, grain, [&](size_t begin, size_t end) {
        if (begin >= end || end > n)
            invalidRanges.fetch_add(1);
        total.fetch_add(end - begin);
        for (size_t i = begin; i < end; ++i) {
            if (covered[i])
                overlaps.fetch_add(1);
            covered[i] = 1;
        }
    });
    REQUIRE(invalidRanges.load() == 0);
    REQUIRE(total.load() == n);
    REQUIRE(overlaps.load() == 0);
    for (size_t i = 0; i < n; ++i)
        REQUIRE(covered[i] == 1);
}

TEST_CASE("Parallel: ranges smaller than the grain run inline once",
          "[parallel]") {
    bool called = false;
    size_t beginCall = 0;
    size_t endCall = 0;
    parallelFor(100, 100000, [&](size_t begin, size_t end) {
        called = true;
        beginCall = begin;
        endCall = end;
    });
    REQUIRE(called);
    REQUIRE(beginCall == 0);
    REQUIRE(endCall == 100);
}

TEST_CASE("Parallel: nested parallelFor does not deadlock and stays correct",
          "[parallel]") {
    // The outer call splits over the pool; the inner call runs inline inside
    // the pool workers (tInPool detection), which must not deadlock.
    const size_t n = 20000;
    std::vector<int> out(n, 0);
    parallelFor(n, 4096, [&](size_t begin, size_t end) {
        parallelFor(end - begin, 1, [&](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i)
                out[begin + i] += 1;
        });
    });
    for (size_t i = 0; i < n; ++i)
        REQUIRE(out[i] == 1);
}

TEST_CASE("Parallel: concurrent parallelFor calls from multiple threads",
          "[parallel]") {
    // Two independent calls racing on the shared pool must both complete and
    // both see correct results.
    const size_t n = 30000;
    std::vector<int> a(n, 0);
    std::vector<int> b(n, 0);

    std::atomic<bool> doneA{false};
    std::atomic<bool> doneB{false};

    std::thread ta([&] {
        parallelFor(n, 2048, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                a[i] = 1;
        });
        doneA = true;
    });
    std::thread tb([&] {
        parallelFor(n, 2048, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                b[i] = 1;
        });
        doneB = true;
    });
    ta.join();
    tb.join();
    REQUIRE(doneA.load());
    REQUIRE(doneB.load());
    for (size_t i = 0; i < n; ++i) {
        REQUIRE(a[i] == 1);
        REQUIRE(b[i] == 1);
    }
}
