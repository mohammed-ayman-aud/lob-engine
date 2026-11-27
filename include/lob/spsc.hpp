// Single-producer / single-consumer lock-free ring buffer (Disruptor-style).
//
//  * head_ (written by the consumer) and tail_ (written by the producer) live on separate
//    cache lines, so the two threads never false-share.
//  * Each side keeps a private *cached* copy of the other side's index and only re-reads the
//    shared atomic when the cached value says the queue looks full/empty. In steady state the
//    hot path touches no shared cache line at all except the slot itself.
//  * Publication uses release stores and acquire loads: the slot write happens-before the
//    consumer's read of it. No read-modify-write atomics, no fences.
#pragma once
#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>

namespace lob {

inline constexpr size_t kCacheLine = 64;

template <class T, size_t N>
class SpscQueue {
    static_assert((N & (N - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>);

    alignas(kCacheLine) std::atomic<size_t> tail_{0};  // next slot to write (producer-owned)
    alignas(kCacheLine) size_t head_cache_ = 0;        // producer's view of head_
    alignas(kCacheLine) std::atomic<size_t> head_{0};  // next slot to read (consumer-owned)
    alignas(kCacheLine) size_t tail_cache_ = 0;        // consumer's view of tail_
    alignas(kCacheLine) T buf_[N];

public:
    bool try_push(const T& v) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_cache_ == N) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t - head_cache_ == N) return false;  // genuinely full
        }
        buf_[t & (N - 1)] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    bool try_pop(T& out) {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) return false;  // genuinely empty
        }
        out = buf_[h & (N - 1)];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    size_t size_approx() const { return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire); }
};

}  // namespace lob
