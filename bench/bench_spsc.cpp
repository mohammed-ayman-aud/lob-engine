// SPSC ring buffer vs a std::mutex + std::deque queue, producer and consumer pinned to
// different cores. Reports throughput (ns per item) and one-way latency percentiles
// (producer stamps rdtsc into the message; consumer measures on arrival).
#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#include "lob/histogram.hpp"
#include "lob/spsc.hpp"

using namespace lob;
using C = std::chrono::steady_clock;

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    pthread_setaffinity_np(pthread_self(), sizeof s, &s);
}

struct MutexQueue {
    std::mutex m;
    std::deque<uint64_t> q;
    bool try_push(uint64_t v) { std::lock_guard g(m); q.push_back(v); return true; }
    bool try_pop(uint64_t& v) { std::lock_guard g(m); if (q.empty()) return false; v = q.front(); q.pop_front(); return true; }
};

template <class Q>
static void bench(const char* name, Q& q, size_t n, double tpn, int pc, int cc, bool paced) {
    Histogram lat;
    std::atomic<bool> go{false};
    std::thread prod([&] {
        pin(pc);
        while (!go.load()) {}
        for (size_t i = 0; i < n; ++i) {
            if (paced) { const uint64_t t = __rdtsc(); while (__rdtsc() - t < uint64_t(200 * tpn)) {} }  // ~200 ns apart: measures latency, not queueing
            while (!q.try_push(__rdtsc())) _mm_pause();
        }
    });
    pin(cc);
    go.store(true);
    const auto t0 = C::now();
    uint64_t v;
    for (size_t got = 0; got < n;) {
        if (q.try_pop(v)) { if (paced) lat.record(uint64_t(double(__rdtsc() - v) / tpn)); ++got; }
    }
    const double ns = std::chrono::duration<double, std::nano>(C::now() - t0).count();
    prod.join();
    if (paced) lat.print(name);
    else std::printf("%-28s %.2f ns/item  (%.1f M items/s)\n", name, ns / double(n), double(n) / ns * 1e3);
}

int main(int argc, char** argv) {
    const int pc = argc > 1 ? std::atoi(argv[1]) : 0, cc = argc > 2 ? std::atoi(argv[2]) : 1;
    const auto t0 = C::now();
    const uint64_t c0 = __rdtsc();
    while (C::now() - t0 < std::chrono::milliseconds(200)) {}
    const double tpn = double(__rdtsc() - c0) / std::chrono::duration<double, std::nano>(C::now() - t0).count();
    std::printf("producer CPU %d, consumer CPU %d, TSC %.3f GHz\n\nthroughput (back-to-back, 20M items):\n", pc, cc, tpn);
    {
        auto q = std::make_unique<SpscQueue<uint64_t, 1 << 14>>();
        bench("spsc ring", *q, 20'000'000, tpn, pc, cc, false);
    }
    { MutexQueue q; bench("mutex + deque", q, 20'000'000, tpn, pc, cc, false); }
    std::printf("\none-way latency (paced, 1M items):\n");
    {
        auto q = std::make_unique<SpscQueue<uint64_t, 1 << 14>>();
        bench("spsc ring", *q, 1'000'000, tpn, pc, cc, true);
    }
    { MutexQueue q; bench("mutex + deque", q, 1'000'000, tpn, pc, cc, true); }
}
