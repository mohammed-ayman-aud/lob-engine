// Open-addressing hash map from uint64 keys to pointers (linear probing, backward-shift
// deletion, power-of-two capacity, load factor <= 0.5). One cache line usually holds the
// whole probe sequence, unlike node-based std::unordered_map.
#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lob {

// splitmix64 finaliser: good default for keys with structure (e.g. prices, multiples of 100).
struct ScrambleHash {
    static uint64_t mix(uint64_t k) {
        k ^= k >> 30; k *= 0xbf58476d1ce4e5b9ULL;
        k ^= k >> 27; k *= 0x94d049bb133111ebULL;
        return k ^ (k >> 31);
    }
};
// Identity: kept only as a measured negative result (`replay --identity-hash`). The idea was
// that sequential ITCH order refs would land in adjacent slots for cache locality. In practice
// some orders rest all day, so the live refs span far more than the table size, wrapped refs
// collide with those survivors, and linear probing builds huge clusters: about 24x slower.
struct IdentityHash {
    static uint64_t mix(uint64_t k) { return k; }
};

template <class V, class H = ScrambleHash>
class FlatMap {
    struct Entry { uint64_t key; V* val; };  // val == nullptr marks an empty slot
    std::vector<Entry> t_;
    size_t mask_, size_ = 0;

    static uint64_t mix(uint64_t k) { return H::mix(k); }
    void rehash() {
        std::vector<Entry> old;
        old.swap(t_);
        t_.assign(old.size() * 2, Entry{0, nullptr});
        mask_ = t_.size() - 1;
        size_ = 0;
        for (auto& e : old) if (e.val) insert(e.key, e.val);
    }

public:
    explicit FlatMap(size_t cap = 1 << 16) : t_(cap, Entry{0, nullptr}), mask_(cap - 1) {}

    V* find(uint64_t k) const {
        for (size_t i = mix(k) & mask_;; i = (i + 1) & mask_) {
            const Entry& e = t_[i];
            if (!e.val) return nullptr;
            if (e.key == k) return e.val;
        }
    }
    // Returns false if the key already exists.
    bool insert(uint64_t k, V* v) {
        if ((size_ + 1) * 2 > t_.size()) rehash();
        for (size_t i = mix(k) & mask_;; i = (i + 1) & mask_) {
            Entry& e = t_[i];
            if (!e.val) { e = {k, v}; ++size_; return true; }
            if (e.key == k) return false;
        }
    }
    bool erase(uint64_t k) {
        size_t i = mix(k) & mask_;
        for (;; i = (i + 1) & mask_) {
            if (!t_[i].val) return false;
            if (t_[i].key == k) break;
        }
        // backward-shift deletion keeps probe chains intact without tombstones
        for (size_t j = (i + 1) & mask_;; j = (j + 1) & mask_) {
            if (!t_[j].val) break;
            const size_t home = mix(t_[j].key) & mask_;
            if (((j - home) & mask_) >= ((j - i) & mask_)) { t_[i] = t_[j]; i = j; }
        }
        t_[i] = Entry{0, nullptr};
        --size_;
        return true;
    }
    size_t size() const { return size_; }
};

// Baseline with the same interface.
template <class V>
class StdMap {
    std::unordered_map<uint64_t, V*> m_;
public:
    explicit StdMap(size_t = 0) {}
    V* find(uint64_t k) const { auto it = m_.find(k); return it == m_.end() ? nullptr : it->second; }
    bool insert(uint64_t k, V* v) { return m_.emplace(k, v).second; }
    bool erase(uint64_t k) { return m_.erase(k) != 0; }
    size_t size() const { return m_.size(); }
};

}  // namespace lob
