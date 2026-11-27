// Matching-engine latency benchmark: pool allocator + open-addressing map ("fast") versus
// new/delete + std::unordered_map ("baseline"), on an identical, seeded order stream.
// Every operation is timed individually with rdtsc and recorded in a histogram, so the
// report is p50 / p99 / p99.9, not a mean.
#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>

#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "lob/histogram.hpp"
#include "lob/matching.hpp"

using namespace lob;

struct Op { uint8_t kind; char side; uint32_t price, qty; uint64_t id; };  // kind 0 = limit, 1 = cancel

static std::vector<Op> make_ops(size_t n, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<Op> ops;
    ops.reserve(n);
    std::vector<uint64_t> ids;
    uint64_t next = 1;
    uint32_t mid = 100000;
    std::geometric_distribution<int> away(0.25);
    for (size_t i = 0; i < n; ++i) {
        if (rng() % 64 == 0) mid += (rng() & 1) ? 10 : -10;
        const uint64_t r = rng() % 100;
        if (r < 35 && !ids.empty()) {  // cancel a random earlier order (may already be filled)
            const size_t k = rng() % ids.size();
            ops.push_back({1, 'B', 0, 0, ids[k]});
            ids[k] = ids.back();
            ids.pop_back();
        } else {
            const char side = (rng() & 1) ? 'B' : 'S';
            // ~10% of orders cross the spread and trade; the rest rest near the inside
            const int ticks = (r % 10 == 0) ? -int(rng() % 3) : 1 + away(rng);
            const uint32_t px = side == 'B' ? mid - 10 * ticks : mid + 10 * ticks;
            ops.push_back({0, side, px, uint32_t(100 * (1 + rng() % 5)), next});
            ids.push_back(next++);
        }
    }
    return ops;
}

template <class T>
static Histogram run(const std::vector<Op>& ops, double tpn, uint64_t& fills) {
    MatchingEngine<T> eng;
    Histogram h;
    fills = 0;
    auto on_fill = [&](const Fill&) { ++fills; };
    for (const Op& op : ops) {
        _mm_lfence();
        const uint64_t c0 = __rdtsc();
        if (op.kind == 0) eng.submit(op.id, op.side, op.price, op.qty, on_fill);
        else eng.cancel(op.id);
        _mm_lfence();
        h.record(uint64_t(double(__rdtsc() - c0) / tpn));
    }
    return h;
}

int main(int argc, char** argv) {
    const size_t n = argc > 1 ? size_t(std::atoll(argv[1])) : 5'000'000;
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(1, &s);
    pthread_setaffinity_np(pthread_self(), sizeof s, &s);

    using C = std::chrono::steady_clock;
    const auto t0 = C::now();
    const uint64_t c0 = __rdtsc();
    while (C::now() - t0 < std::chrono::milliseconds(200)) {}
    const double tpn = double(__rdtsc() - c0) / std::chrono::duration<double, std::nano>(C::now() - t0).count();

    // timer overhead: an empty timed region
    Histogram empty;
    for (int i = 0; i < 1000000; ++i) { _mm_lfence(); const uint64_t a = __rdtsc(); _mm_lfence(); empty.record(uint64_t(double(__rdtsc() - a) / tpn)); }

    const auto ops = make_ops(n, 42);
    uint64_t f1, f2;
    run<Fast>(ops, tpn, f1);  // warm-up pass
    const Histogram fast = run<Fast>(ops, tpn, f1);
    const Histogram base = run<Baseline>(ops, tpn, f2);
    if (f1 != f2) { std::printf("MISMATCH: fills %llu vs %llu\n", (unsigned long long)f1, (unsigned long long)f2); return 1; }

    std::printf("%zu operations, %llu fills, TSC %.3f GHz, pinned to CPU 1\n", n, (unsigned long long)f1, tpn);
    empty.print("timer overhead (empty)");
    fast.print("fast (pool + flat map)");
    base.print("baseline (new + unordered_map)");
    for (double p : {50.0, 99.0, 99.9})
        std::printf("p%-5g reduction: %.0f%%\n", p, 100.0 * (1.0 - double(fast.percentile(p)) / double(base.percentile(p))));
}
