#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <stdexcept>
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

TEST_CASE("Parallel: n == 0 invokes nothing", "[parallel]") {
    bool called = false;
    parallelFor(0, 1, [&](size_t, size_t) { called = true; });
    REQUIRE_FALSE(called);
}

TEST_CASE("Parallel: single-chunk range runs on the calling thread",
          "[parallel]") {
    // The n <= grain fast path is inline by contract: fn must run on the
    // caller, not on a pool worker and not through a second chunk.
    const std::thread::id self = std::this_thread::get_id();
    std::thread::id observed = std::thread::id();
    size_t calls = 0;
    parallelFor(512, 512, [&](size_t begin, size_t end) {
        ++calls;
        observed = std::this_thread::get_id();
        REQUIRE(begin == 0);
        REQUIRE(end == 512);
    });
    REQUIRE(calls == 1);
    REQUIRE(observed == self);
}

TEST_CASE("Parallel: template and type-erased dispatch agree chunk for chunk",
          "[parallel]") {
    // Guards the IM-3 contract: the inline fast path moved into the header
    // template must not change which chunks fn sees, nor their results.
    const size_t n = 200000;
    const size_t grain = 4096;
    std::vector<float> viaTemplate(n, 0.0f);
    std::vector<float> viaFunction(n, 0.0f);
    std::vector<float> viaErased(n, 0.0f);
    const auto fill = [](std::vector<float>& out) {
        return [&out](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                out[i] = static_cast<float>((i * 2654435761u) % 997u) / 997.0f;
        };
    };
    parallelFor(n, grain, fill(viaTemplate));
    std::function<void(size_t, size_t)> typed = fill(viaFunction);
    parallelFor(n, grain, typed);
    lrc::parallel::detail::parallelForErased(n, grain, fill(viaErased));
    REQUIRE(std::equal(viaTemplate.begin(), viaTemplate.end(),
                       viaFunction.begin()));
    REQUIRE(std::equal(viaTemplate.begin(), viaTemplate.end(),
                       viaErased.begin()));

    // The same agreement must hold for a range that fits in one chunk.
    std::vector<int> smallTemplate(1000, 0);
    std::vector<int> smallErased(1000, 0);
    parallelFor(smallTemplate.size(), 4096, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i)
            smallTemplate[i] = 1;
    });
    lrc::parallel::detail::parallelForErased(
        1000, 4096, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                smallErased[i] = 1;
        });
    REQUIRE(std::equal(smallTemplate.begin(), smallTemplate.end(),
                       smallErased.begin()));
}

TEST_CASE("Parallel: exception from the inline path reaches the caller",
          "[parallel]") {
    REQUIRE_THROWS_AS(parallelFor(10, 1024, [](size_t, size_t) {
                          throw std::runtime_error("inline boom");
                      }),
                      std::runtime_error);
}

TEST_CASE("Parallel: a throwing chunk does not hang or break the pool",
          "[parallel]") {
    // A chunk may throw from the caller thread (rethrown) or from a worker
    // (swallowed by the worker loop). Either way the call must return only
    // after in-flight chunks finish, and the pool must stay usable.
    bool callerSawException = false;
    try {
        parallelFor(400000, 128, [](size_t, size_t) {
            throw std::runtime_error("chunk boom");
        });
    } catch (const std::runtime_error&) {
        callerSawException = true;
    }
    (void)callerSawException;
    std::vector<int> out(100000, 0);
    parallelFor(out.size(), 512, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i)
            out[i] = 1;
    });
    for (size_t i = 0; i < out.size(); ++i)
        REQUIRE(out[i] == 1);
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
