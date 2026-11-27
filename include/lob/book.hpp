// Limit order book with intrusive price levels.
//
//  * Order    — one resting order; doubly linked into its price level's FIFO queue.
//  * Level    — all orders at one price, in time priority (head = oldest).
//  * Side<>   — price -> Level hash map for O(1) lookup, plus a price-sorted ladder of
//               levels kept with the best price at the *back*, so the hot inside of the
//               book is touched with O(1) amortised vector operations.
//
// Cost model: execute / cancel / delete of any order is O(1) (hash lookup + unlink).
// Adding at an existing price is O(1). Only creating or removing a *price level* touches
// the ladder: O(log L) search plus a shift of the levels better than it, which is small
// because activity concentrates at the inside.
#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "flat_map.hpp"
#include "pool.hpp"

namespace lob {

struct Level;

// 48 bytes: the fields touched on every lookup (ref, links, shares, level) share a cache line.
struct Order {
    uint64_t ref;
    Order* prev;
    Order* next;
    Level* level;
    uint32_t shares;
    uint32_t price;
    uint16_t locate;
    char side;  // 'B' or 'S'
};

struct Level {
    uint32_t price;
    uint32_t count;
    uint64_t shares;
    Order* head;
    Order* tail;
};

template <class LevelAlloc, template <class> class Map>
class Side {
    Map<Level> by_price_{32};  // grows on demand; most books have few levels
    std::vector<Level*> ladder_;  // sorted, best price at the back
    LevelAlloc* alloc_;
    bool bid_;

    // "a is better than b" for this side
    bool better(uint32_t a, uint32_t b) const { return bid_ ? a > b : a < b; }
    auto pos(uint32_t price) {  // first element not worse... ladder ascends in "goodness"
        return std::lower_bound(ladder_.begin(), ladder_.end(), price,
                                [this](const Level* l, uint32_t p) { return better(p, l->price); });
    }

public:
    Side(LevelAlloc* a, bool bid) : alloc_(a), bid_(bid) {}

    Level* level_for(uint32_t price) {
        if (Level* l = by_price_.find(price)) return l;
        Level* l = alloc_->make(Level{price, 0, 0, nullptr, nullptr});
        by_price_.insert(price, l);
        ladder_.insert(pos(price), l);
        return l;
    }
    void remove_level(Level* l) {
        by_price_.erase(l->price);
        auto it = pos(l->price);
        ladder_.erase(it);
        alloc_->destroy(l);
    }
    void append(Order* o) {
        Level* l = level_for(o->price);
        o->level = l;
        o->prev = l->tail;
        o->next = nullptr;
        if (l->tail) l->tail->next = o; else l->head = o;
        l->tail = o;
        l->count++;
        l->shares += o->shares;
    }
    // Unlink `o`; drops the level when it empties.
    void unlink(Order* o) {
        Level* l = o->level;
        if (o->prev) o->prev->next = o->next; else l->head = o->next;
        if (o->next) o->next->prev = o->prev; else l->tail = o->prev;
        l->count--;
        l->shares -= o->shares;
        if (l->count == 0) remove_level(l);
    }
    Level* best() const { return ladder_.empty() ? nullptr : ladder_.back(); }
    // i-th best level (0 = best), or nullptr
    Level* depth(size_t i) const { return i < ladder_.size() ? ladder_[ladder_.size() - 1 - i] : nullptr; }
    size_t levels() const { return ladder_.size(); }

    // Full structural check (tests and fuzzing): ladder strictly ordered, the hash map agrees
    // with the ladder, and every level's FIFO links, count and share total are consistent.
    // Returns the number of orders on this side, or -1 if anything is inconsistent.
    long validate() const {
        if (by_price_.size() != ladder_.size()) return -1;
        long orders = 0;
        for (size_t i = 0; i < ladder_.size(); ++i) {
            const Level* l = ladder_[i];
            if (by_price_.find(l->price) != l) return -1;
            if (i > 0 && !better(l->price, ladder_[i - 1]->price)) return -1;
            if (l->count == 0 || !l->head || l->head->prev || l->tail->next) return -1;
            uint64_t shares = 0;
            uint32_t n = 0;
            for (const Order* o = l->head; o; o = o->next) {
                if (o->level != l || o->price != l->price || (o->next && o->next->prev != o)) return -1;
                shares += o->shares;
                ++n;
                if (!o->next && o != l->tail) return -1;
            }
            if (n != l->count || shares != l->shares) return -1;
            orders += n;
        }
        return orders;
    }
    bool is_bid() const { return bid_; }
};

template <class V> using HashedMap = FlatMap<V, ScrambleHash>;
template <class V> using IdentityMap = FlatMap<V, IdentityHash>;

// Policy bundle: allocators, the order-ref map (OM) and the price-level map (M).
template <class OrderAlloc, class LevelAlloc, template <class> class OrderMap, template <class> class LevelMap>
struct BookTypes {
    using OA = OrderAlloc;
    using LA = LevelAlloc;
    template <class V> using OM = OrderMap<V>;
    template <class V> using M = LevelMap<V>;
};

using Fast = BookTypes<Pool<Order>, Pool<Level>, HashedMap, HashedMap>;
using FastIdentity = BookTypes<Pool<Order>, Pool<Level>, IdentityMap, HashedMap>;  // negative result, see flat_map.hpp
using Baseline = BookTypes<HeapAlloc<Order>, HeapAlloc<Level>, StdMap, StdMap>;

// One instrument's book.
template <class T = Fast>
struct Book {
    Side<typename T::LA, T::template M> bids, asks;
    explicit Book(typename T::LA* la) : bids(la, true), asks(la, false) {}
    auto& side(char s) { return s == 'B' ? bids : asks; }
};

}  // namespace lob
