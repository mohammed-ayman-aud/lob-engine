// Unit tests (no framework dependency): ITCH decoding, book reconstruction semantics,
// matching-engine priority rules, the flat map against std::unordered_map, the SPSC queue
// under two threads, and the histogram.
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "lob/book_builder.hpp"
#include "lob/histogram.hpp"
#include "lob/matching.hpp"
#include "lob/spsc.hpp"

using namespace lob;
static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- tiny ITCH encoder -----------------------------------------------------------
struct Enc {
    std::vector<uint8_t> buf;
    std::vector<uint8_t> cur;
    void u8(uint8_t v) { cur.push_back(v); }
    void be(uint64_t v, int n) { for (int i = n - 1; i >= 0; --i) cur.push_back(uint8_t(v >> (8 * i))); }
    void str(const char* s, int n) { for (int i = 0; i < n; ++i) cur.push_back(i < (int)std::strlen(s) ? s[i] : ' '); }
    void hdr(char t, uint16_t loc) { cur.clear(); u8(t); be(loc, 2); be(0, 2); be(1, 6); }
    void end() { be_len(); }
    void be_len() { buf.push_back(uint8_t(cur.size() >> 8)); buf.push_back(uint8_t(cur.size())); buf.insert(buf.end(), cur.begin(), cur.end()); }
    void dir(uint16_t loc, const char* sym) { hdr('R', loc); str(sym, 8); for (int i = 0; i < 20; ++i) u8('N'); end(); }
    void add(uint16_t loc, uint64_t ref, char side, uint32_t sh, uint32_t px) { hdr('A', loc); be(ref, 8); u8(side); be(sh, 4); str("X", 8); be(px, 4); end(); }
    void exec(uint64_t ref, uint32_t sh) { hdr('E', 1); be(ref, 8); be(sh, 4); be(7, 8); end(); }
    void cancel(uint64_t ref, uint32_t sh) { hdr('X', 1); be(ref, 8); be(sh, 4); end(); }
    void del(uint64_t ref) { hdr('D', 1); be(ref, 8); end(); }
    void replace(uint64_t o, uint64_t n, uint32_t sh, uint32_t px) { hdr('U', 1); be(o, 8); be(n, 8); be(sh, 4); be(px, 4); end(); }
};

static void test_itch_sizes() {
    Enc e;
    e.dir(1, "AAPL"); e.add(1, 1, 'B', 100, 1000); e.exec(1, 1); e.cancel(1, 1); e.del(1); e.replace(1, 2, 1, 1);
    const size_t want[] = {39, 36, 31, 23, 19, 35};
    size_t off = 0;
    for (size_t w : want) { CHECK(itch::be16(e.buf.data() + off) == w); off += 2 + w; }
    itch::NullVisitor v;
    auto r = itch::walk(e.buf.data(), e.buf.size(), v);
    CHECK(r.status == itch::Status::Ok && r.messages == 6);
    // truncated stream stops cleanly at the last whole frame
    r = itch::walk(e.buf.data(), e.buf.size() - 3, v);
    CHECK(r.status == itch::Status::Truncated && r.messages == 5);
    // wrong length for a known type is rejected, not read past
    uint8_t bad[] = {0, 5, 'A', 0, 1, 0, 0};
    r = itch::walk(bad, sizeof bad, v);
    CHECK(r.status == itch::Status::Truncated || r.status == itch::Status::BadLength);
}

static void test_book() {
    Enc e;
    e.dir(1, "TEST");
    e.add(1, 1, 'B', 100, 10000);
    e.add(1, 2, 'B', 200, 10000);  // same level, behind order 1
    e.add(1, 3, 'B', 300, 10100);  // better bid
    e.add(1, 4, 'S', 50, 10200);
    e.exec(3, 100);                // partial
    e.cancel(1, 40);               // partial
    e.replace(2, 5, 500, 9900);    // moves to a new, worse level
    BookBuilder<Fast> b;
    itch::walk(e.buf.data(), e.buf.size(), b);
    CHECK(b.validate());
    auto* bk = b.find_book(1);
    CHECK(bk->bids.best()->price == 10100 && bk->bids.best()->shares == 200);
    CHECK(bk->bids.depth(1)->price == 10000 && bk->bids.depth(1)->shares == 60 && bk->bids.depth(1)->count == 1);
    CHECK(bk->bids.depth(2)->price == 9900 && bk->bids.depth(2)->head->ref == 5);
    CHECK(bk->asks.best()->price == 10200);
    CHECK(std::string(b.symbol(1)) == "TEST");
    Enc e2;
    e2.exec(3, 200); e2.del(1); e2.del(5); e2.del(4); e2.del(999);
    itch::walk(e2.buf.data(), e2.buf.size(), b);
    CHECK(b.validate() && b.live_orders() == 0 && bk->bids.levels() == 0 && bk->asks.levels() == 0);
    CHECK(b.stats.unknown_ref == 1);
}

template <class T>
static void test_matching() {
    MatchingEngine<T> m;
    std::vector<Fill> f;
    auto rec = [&](const Fill& x) { f.push_back(x); };
    m.submit(1, 'S', 101, 100, rec);
    m.submit(2, 'S', 101, 100, rec);  // same price, later: lower priority
    m.submit(3, 'S', 100, 50, rec);   // better price
    CHECK(f.empty() && m.best_ask()->price == 100);
    // buy 180 @ 101: takes 50 @100 (id 3), then 100 from id 1, then 30 from id 2
    const uint32_t rest = m.submit(4, 'B', 101, 180, rec);
    CHECK(rest == 0 && f.size() == 3);
    CHECK(f[0].maker == 3 && f[0].price == 100 && f[0].qty == 50);
    CHECK(f[1].maker == 1 && f[1].qty == 100);
    CHECK(f[2].maker == 2 && f[2].qty == 30);
    CHECK(m.best_ask()->shares == 70);
    // a non-crossing bid rests; cancel removes it
    CHECK(m.submit(5, 'B', 99, 10, rec) == 10 && m.best_bid()->price == 99);
    CHECK(m.cancel(5) && !m.best_bid() && !m.cancel(5));
    // IOC never rests; market sweeps through price levels
    f.clear();
    CHECK(m.submit(6, 'B', 100, 10, OrderType::IOC, rec) == 0 && f.empty());
    m.submit(7, 'S', 105, 100, rec);
    CHECK(m.submit(8, 'B', 0, 120, OrderType::Market, rec) == 0 && f.size() == 2 && f[1].price == 105 && f[1].qty == 50);
    CHECK(m.resting() == 1 && m.best_ask()->shares == 50);
}

static void test_flat_map() {
    FlatMap<int> fm(8);
    std::unordered_map<uint64_t, int*> ref;
    std::mt19937_64 rng(7);
    std::vector<int> vals(1);
    int* p = &vals[0];
    for (int i = 0; i < 200000; ++i) {
        const uint64_t k = rng() % 5000;
        if (rng() % 3) { bool a = fm.insert(k, p), b = ref.emplace(k, p).second; if (a != b) { CHECK(a == b); break; } }
        else { bool a = fm.erase(k), b = ref.erase(k); if (a != b) { CHECK(a == b); break; } }
        if (fm.size() != ref.size()) { CHECK(fm.size() == ref.size()); break; }
    }
    for (uint64_t k = 0; k < 5000; ++k) if ((fm.find(k) != nullptr) != ref.count(k)) { CHECK(false); break; }
    CHECK(true);
}

static void test_spsc() {
    auto q = std::make_unique<SpscQueue<uint64_t, 1024>>();
    const uint64_t n = 5'000'000;
    std::thread prod([&] { for (uint64_t i = 1; i <= n; ++i) while (!q->try_push(i)) {} });
    uint64_t expect = 1, v;
    bool ordered = true;
    while (expect <= n) if (q->try_pop(v)) { if (v != expect) ordered = false; ++expect; }
    prod.join();
    CHECK(ordered && !q->try_pop(v));
}

static void test_histogram() {
    Histogram h;
    for (uint64_t i = 1; i <= 10000; ++i) h.record(i);
    CHECK(h.percentile(50) >= 4900 && h.percentile(50) <= 5100);
    CHECK(h.percentile(99) >= 9800 && h.percentile(99) <= 10000);
    CHECK(h.max() == 10000 && h.count() == 10000);
}

int main() {
    test_itch_sizes();
    test_book();
    test_matching<Fast>();
    test_matching<Baseline>();
    test_flat_map();
    test_spsc();
    test_histogram();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
