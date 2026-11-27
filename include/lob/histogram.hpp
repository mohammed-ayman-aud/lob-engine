// Log-linear latency histogram (HdrHistogram-style bucketing, ~1.6% relative precision).
// Values < 64 are exact; above that each power of two is split into 64 linear sub-buckets.
// Recording is a couple of shifts and an increment, so it can sit inside a timed loop.
#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>

namespace lob {

class Histogram {
    static constexpr int kSub = 6;  // 2^6 = 64 sub-buckets per octave
    std::array<uint64_t, 64 << kSub> counts_{};
    uint64_t total_ = 0, max_ = 0, sum_ = 0;

    static size_t index(uint64_t v) {
        if (v < (1u << kSub)) return v;
        const int msb = 63 - std::countl_zero(v);
        const int shift = msb - kSub;
        return (size_t(shift + 1) << kSub) + ((v >> shift) & ((1u << kSub) - 1));
    }
    static uint64_t value_at(size_t i) {  // upper edge of bucket i
        if (i < (1u << kSub)) return i;
        const int shift = int(i >> kSub) - 1;
        const uint64_t base = (uint64_t(1) << kSub) | (i & ((1u << kSub) - 1));
        return ((base + 1) << shift) - 1;
    }

public:
    void record(uint64_t v) {
        counts_[index(v)]++;
        total_++;
        sum_ += v;
        if (v > max_) max_ = v;
    }
    uint64_t count() const { return total_; }
    uint64_t max() const { return max_; }
    double mean() const { return total_ ? double(sum_) / total_ : 0; }
    uint64_t percentile(double p) const {
        if (!total_) return 0;
        const uint64_t want = uint64_t(p / 100.0 * double(total_) + 0.5);
        uint64_t seen = 0;
        for (size_t i = 0; i < counts_.size(); ++i) {
            seen += counts_[i];
            if (seen >= want && counts_[i]) return value_at(i) < max_ ? value_at(i) : max_;
        }
        return max_;
    }
    void print(const char* label, const char* unit = "ns") const {
        std::printf("%-28s n=%-10llu p50=%llu p90=%llu p99=%llu p99.9=%llu max=%llu %s\n", label,
                    (unsigned long long)total_, (unsigned long long)percentile(50), (unsigned long long)percentile(90),
                    (unsigned long long)percentile(99), (unsigned long long)percentile(99.9),
                    (unsigned long long)max_, unit);
    }
};

}  // namespace lob
