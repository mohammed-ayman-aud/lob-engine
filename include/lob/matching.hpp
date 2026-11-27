// Price-time-priority matching engine for one instrument, built on the same intrusive
// levels as the ITCH book. Incoming orders cross against the opposite side best-first,
// oldest-first at each price; any limit remainder rests on the book.
#pragma once
#include <cstdint>

#include "book.hpp"

namespace lob {

struct Fill {
    uint64_t taker, maker;
    uint32_t price, qty;
};

enum class OrderType : uint8_t { Limit, Market, IOC };

template <class T = Fast>
class MatchingEngine {
public:
    MatchingEngine() : book_(&levels_) {}

    // Returns quantity left resting on the book (0 if fully filled, cancelled or not resting).
    // `on_fill(const Fill&)` is called for each execution, in execution order.
    template <class F>
    uint32_t submit(uint64_t id, char side, uint32_t price, uint32_t qty, OrderType type, F&& on_fill) {
        auto& opp = side == 'B' ? book_.asks : book_.bids;
        while (qty > 0) {
            Level* best = opp.best();
            if (!best) break;
            if (type != OrderType::Market && (side == 'B' ? best->price > price : best->price < price)) break;
            while (qty > 0 && best->head) {
                Order* maker = best->head;
                const uint32_t q = qty < maker->shares ? qty : maker->shares;
                on_fill(Fill{id, maker->ref, best->price, q});
                qty -= q;
                if (q == maker->shares) {
                    const bool last = best->count == 1;
                    erase(maker, opp);
                    if (last) break;  // level object is gone
                } else {
                    maker->shares -= q;
                    best->shares -= q;
                }
            }
        }
        if (qty == 0 || type != OrderType::Limit) return 0;
        if (orders_.find(id)) return 0;  // duplicate id: reject the remainder
        Order* o = pool_.make(Order{id, nullptr, nullptr, nullptr, qty, price, 0, side});
        orders_.insert(id, o);
        book_.side(side).append(o);
        return qty;
    }
    template <class F>
    uint32_t submit(uint64_t id, char side, uint32_t price, uint32_t qty, F&& on_fill) {
        return submit(id, side, price, qty, OrderType::Limit, on_fill);
    }

    bool cancel(uint64_t id) {
        Order* o = orders_.find(id);
        if (!o) return false;
        erase(o, book_.side(o->side));
        return true;
    }

    const Level* best_bid() const { return book_.bids.best(); }
    const Level* best_ask() const { return book_.asks.best(); }
    size_t resting() const { return orders_.size(); }

private:
    template <class S>
    void erase(Order* o, S& side) {
        side.unlink(o);
        orders_.erase(o->ref);
        pool_.destroy(o);
    }
    typename T::OA pool_;
    typename T::LA levels_;
    typename T::template OM<Order> orders_{1 << 16};
    Book<T> book_;
};

}  // namespace lob
