// Rebuilds every instrument's full-depth order book from an ITCH 5.0 stream.
// Plugs into itch::walk() as the visitor.
#pragma once
#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "book.hpp"
#include "itch.hpp"

namespace lob {

template <class T = Fast>
class BookBuilder {
public:
    struct Stats {
        uint64_t adds = 0, executes = 0, cancels = 0, deletes = 0, replaces = 0, trades = 0, other = 0;
        uint64_t unknown_ref = 0;    // reference to an order we never saw (feed joined mid-session)
        uint64_t bad_quantity = 0;   // execute/cancel larger than the resting size
    } stats;

    BookBuilder() : books_(1 << 16) {}

    Book<T>* book(uint16_t locate) {
        auto& b = books_[locate];
        if (!b) b = std::make_unique<Book<T>>(&levels_);
        return b.get();
    }
    const Book<T>* find_book(uint16_t locate) const { return books_[locate].get(); }
    const char* symbol(uint16_t locate) const { return symbols_[locate].data(); }
    size_t live_orders() const { return orders_.size(); }

    // Every book structurally valid and every live order reachable from exactly one level.
    bool validate() const {
        long total = 0;
        for (const auto& b : books_) {
            if (!b) continue;
            const long nb = b->bids.validate(), na = b->asks.validate();
            if (nb < 0 || na < 0) return false;
            total += nb + na;
        }
        return size_t(total) == orders_.size();
    }

    // ---- itch visitor ------------------------------------------------------------
    void on_directory(const itch::StockDirectory& m) {
        auto& s = symbols_[m.h.locate];
        for (int i = 0; i < 8; ++i) s[i] = m.stock[i] == ' ' ? '\0' : m.stock[i];
        s[8] = '\0';
    }
    void on_add(const itch::AddOrder& m) {
        stats.adds++;
        if (m.side != 'B' && m.side != 'S') { stats.bad_quantity++; return; }
        if (orders_.find(m.ref)) { stats.bad_quantity++; return; }  // duplicate ref: ignore
        Order* o = order_pool_.make(Order{m.ref, nullptr, nullptr, nullptr, m.shares, m.price, m.h.locate, m.side});
        orders_.insert(m.ref, o);
        book(m.h.locate)->side(m.side).append(o);
    }
    void on_executed(const itch::OrderExecuted& m) { stats.executes++; reduce(m.ref, m.shares); }
    void on_executed_price(const itch::OrderExecutedWithPrice& m) { stats.executes++; reduce(m.ref, m.shares); }
    void on_cancel(const itch::OrderCancel& m) { stats.cancels++; reduce(m.ref, m.shares); }
    void on_delete(const itch::OrderDelete& m) {
        stats.deletes++;
        Order* o = orders_.find(m.ref);
        if (!o) { stats.unknown_ref++; return; }
        remove(o);
    }
    void on_replace(const itch::OrderReplace& m) {
        stats.replaces++;
        Order* o = orders_.find(m.old_ref);
        if (!o || orders_.find(m.new_ref)) { stats.unknown_ref++; return; }
        // replace loses time priority: unlink, then re-add under the new ref/price/size
        const uint16_t loc = o->locate;
        const char side = o->side;
        remove(o);
        Order* n = order_pool_.make(Order{m.new_ref, nullptr, nullptr, nullptr, m.shares, m.price, loc, side});
        orders_.insert(m.new_ref, n);
        book(loc)->side(side).append(n);
    }
    void on_trade(const itch::Trade&) { stats.trades++; }  // non-displayed: no book effect
    void on_other(char) { stats.other++; }

    // Full-depth snapshot of one book side, best first: "price shares count" per line.
    template <class F>
    void for_each_level(uint16_t locate, char side, size_t max_depth, F&& f) const {
        const Book<T>* b = find_book(locate);
        if (!b) return;
        auto& s = side == 'B' ? b->bids : b->asks;
        for (size_t i = 0; i < max_depth; ++i) {
            const Level* l = s.depth(i);
            if (!l) break;
            f(*l);
        }
    }

private:
    void reduce(uint64_t ref, uint32_t shares) {
        Order* o = orders_.find(ref);
        if (!o) { stats.unknown_ref++; return; }
        if (shares > o->shares) { stats.bad_quantity++; shares = o->shares; }
        if (shares == o->shares) { remove(o); return; }
        o->shares -= shares;
        o->level->shares -= shares;
    }
    void remove(Order* o) {
        book(o->locate)->side(o->side).unlink(o);
        orders_.erase(o->ref);
        order_pool_.destroy(o);
    }

    typename T::OA order_pool_;
    typename T::LA levels_;
    typename T::template OM<Order> orders_{1 << 20};
    std::vector<std::unique_ptr<Book<T>>> books_;
    std::array<std::array<char, 9>, 1 << 16> symbols_{};
};

}  // namespace lob
