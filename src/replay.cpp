// replay: rebuild full-depth books from an ITCH 5.0 file and report throughput/latency.
//
//   replay FILE                 single thread: decode + apply, messages/sec
//   replay FILE --latency       also time every message (rdtsc), p50/p99/p99.9 in ns
//   replay FILE --pipeline      two pinned threads: feed (framing) -> SPSC queue -> book thread
//   replay FILE --baseline      same, with new/delete + std::unordered_map instead of pool + flat map
//   replay FILE --identity-hash ablation: identity-hash the order refs (a measured negative result)
//   replay FILE --dump OUT      write the top levels of every book (for differential tests)
//               [--depth N]     levels per side in the dump (default 5)
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <x86intrin.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "lob/book_builder.hpp"
#include "lob/histogram.hpp"
#include "lob/itch.hpp"
#include "lob/spsc.hpp"

using namespace lob;
using Clock = std::chrono::steady_clock;

static void pin(int cpu) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    pthread_setaffinity_np(pthread_self(), sizeof s, &s);
}

static double tsc_per_ns() {
    const auto t0 = Clock::now();
    const uint64_t c0 = __rdtsc();
    while (Clock::now() - t0 < std::chrono::milliseconds(200)) {}
    const uint64_t c1 = __rdtsc();
    return double(c1 - c0) / std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
}

struct Mapped {
    const uint8_t* p = nullptr;
    size_t n = 0;
    explicit Mapped(const char* path) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) { std::perror(path); std::exit(1); }
        struct stat st;
        fstat(fd, &st);
        n = size_t(st.st_size);
        p = static_cast<const uint8_t*>(mmap(nullptr, n, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0));
        if (p == MAP_FAILED) { std::perror("mmap"); std::exit(1); }
        close(fd);
    }
};

struct Frame { uint32_t off; uint16_t len; };

template <class T>
int run(const Mapped& f, bool latency, bool pipeline, const char* dump, size_t depth) {
    auto b = std::make_unique<BookBuilder<T>>();
    size_t msgs = 0;
    double secs = 0;

    if (pipeline) {
        // feed thread: framing only (what a network handler would do), book thread: decode + apply
        auto q = std::make_unique<SpscQueue<Frame, 1 << 16>>();
        std::atomic<bool> done{false};
        const auto t0 = Clock::now();
        std::thread feed([&] {
            pin(0);
            size_t off = 0;
            while (off + 2 <= f.n) {
                const uint16_t len = itch::be16(f.p + off);
                if (off + 2 + len > f.n) break;
                const Frame fr{uint32_t(off + 2), len};
                while (!q->try_push(fr)) _mm_pause();
                off += 2 + len;
            }
            done.store(true, std::memory_order_release);
        });
        pin(1);
        Frame fr;
        for (;;) {
            if (q->try_pop(fr)) {
                itch::decode(f.p + fr.off, fr.len, *b);
                ++msgs;
            } else if (done.load(std::memory_order_acquire)) {
                while (q->try_pop(fr)) { itch::decode(f.p + fr.off, fr.len, *b); ++msgs; }
                break;
            } else {
                _mm_pause();
            }
        }
        feed.join();
        secs = std::chrono::duration<double>(Clock::now() - t0).count();
    } else if (latency) {
        pin(1);
        const double tpn = tsc_per_ns();
        Histogram h;
        size_t off = 0;
        const auto t0 = Clock::now();
        while (off + 2 <= f.n) {
            const uint16_t len = itch::be16(f.p + off);
            if (off + 2 + len > f.n) break;
            _mm_lfence();
            const uint64_t c0 = __rdtsc();
            itch::decode(f.p + off + 2, len, *b);
            _mm_lfence();
            const uint64_t c1 = __rdtsc();
            h.record(uint64_t(double(c1 - c0) / tpn));
            off += 2 + len;
            ++msgs;
        }
        secs = std::chrono::duration<double>(Clock::now() - t0).count();
        h.print("per-message decode+apply");
    } else {
        pin(1);
        const auto t0 = Clock::now();
        auto r = itch::walk(f.p, f.n, *b);
        secs = std::chrono::duration<double>(Clock::now() - t0).count();
        msgs = r.messages;
        if (r.status != itch::Status::Ok) std::fprintf(stderr, "stopped early: status %d at byte %zu\n", int(r.status), r.consumed);
    }

    auto& s = b->stats;
    std::printf("%zu messages in %.3f s = %.2f M msg/s (%.1f ns/msg)\n", msgs, secs, msgs / secs / 1e6, secs * 1e9 / msgs);
    std::printf("adds %llu  executes %llu  cancels %llu  deletes %llu  replaces %llu  trades %llu  other %llu  "
                "unknown-ref %llu  live orders %zu\n",
                (unsigned long long)s.adds, (unsigned long long)s.executes, (unsigned long long)s.cancels,
                (unsigned long long)s.deletes, (unsigned long long)s.replaces, (unsigned long long)s.trades,
                (unsigned long long)s.other, (unsigned long long)s.unknown_ref, b->live_orders());
    if (dump) {
        FILE* out = std::fopen(dump, "w");
        // locates sorted ascending, like the reference implementation
        for (int loc = 1; loc < (1 << 16); ++loc) {
            const char* sym = b->symbol(uint16_t(loc));
            if (!sym[0]) continue;
            for (char side : {'B', 'S'})
                b->for_each_level(uint16_t(loc), side, depth, [&](const Level& l) {
                    std::fprintf(out, "%s %c %u %llu %u\n", sym, side, l.price, (unsigned long long)l.shares, l.count);
                });
        }
        std::fclose(out);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: replay FILE [--latency] [--pipeline] [--baseline] [--dump OUT [--depth N]]\n"); return 2; }
    bool latency = false, pipeline = false, baseline = false, identity = false;
    const char* dump = nullptr;
    size_t depth = 5;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--latency")) latency = true;
        else if (!std::strcmp(argv[i], "--pipeline")) pipeline = true;
        else if (!std::strcmp(argv[i], "--baseline")) baseline = true;
        else if (!std::strcmp(argv[i], "--identity-hash")) identity = true;
        else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!std::strcmp(argv[i], "--depth") && i + 1 < argc) depth = size_t(std::atoll(argv[++i]));
    }
    Mapped f(argv[1]);
    if (baseline) return run<Baseline>(f, latency, pipeline, dump, depth);
    if (identity) return run<FastIdentity>(f, latency, pipeline, dump, depth);
    return run<Fast>(f, latency, pipeline, dump, depth);
}
