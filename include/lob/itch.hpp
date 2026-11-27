// NASDAQ TotalView-ITCH 5.0: zero-copy decoding of the messages that affect the book.
//
// Files from NASDAQ are a sequence of [u16 big-endian length][message] frames.
// Decoding reads fields straight out of the input buffer (no copies, no allocation);
// every read is bounds-checked against the frame length, so malformed input is
// reported as an error instead of being read past the end (this is what the fuzzer checks).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lob::itch {

inline uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint64_t be48(const uint8_t* p) { return uint64_t(be16(p)) << 32 | be32(p + 2); }
inline uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

// Header common to every message: type(1) locate(2) tracking(2) timestamp(6) = 11 bytes.
struct Header {
    char type;
    uint16_t locate;
    uint64_t timestamp_ns;  // nanoseconds since midnight
};

struct AddOrder { Header h; uint64_t ref; char side; uint32_t shares; char stock[8]; uint32_t price; };
struct OrderExecuted { Header h; uint64_t ref; uint32_t shares; uint64_t match; };
struct OrderExecutedWithPrice { Header h; uint64_t ref; uint32_t shares; uint64_t match; uint32_t price; };
struct OrderCancel { Header h; uint64_t ref; uint32_t shares; };
struct OrderDelete { Header h; uint64_t ref; };
struct OrderReplace { Header h; uint64_t old_ref; uint64_t new_ref; uint32_t shares; uint32_t price; };
struct Trade { Header h; uint64_t ref; char side; uint32_t shares; char stock[8]; uint32_t price; uint64_t match; };
struct StockDirectory { Header h; char stock[8]; };

// Exact on-wire sizes from the ITCH 5.0 specification (0 = unknown type).
constexpr size_t message_size(char t) {
    switch (t) {
        case 'S': return 12; case 'R': return 39; case 'H': return 25; case 'Y': return 20;
        case 'L': return 26; case 'V': return 35; case 'W': return 12; case 'K': return 28;
        case 'J': return 35; case 'h': return 21; case 'A': return 36; case 'F': return 40;
        case 'E': return 31; case 'C': return 36; case 'X': return 23; case 'D': return 19;
        case 'U': return 35; case 'P': return 44; case 'Q': return 40; case 'B': return 19;
        case 'I': return 50; case 'N': return 20; case 'O': return 48;
        default: return 0;
    }
}

enum class Status { Ok, Truncated, BadLength, UnknownType };

// Visitor interface: implement any subset of on_* (default no-ops via CRTP-free overloads).
struct NullVisitor {
    void on_add(const AddOrder&) {}
    void on_executed(const OrderExecuted&) {}
    void on_executed_price(const OrderExecutedWithPrice&) {}
    void on_cancel(const OrderCancel&) {}
    void on_delete(const OrderDelete&) {}
    void on_replace(const OrderReplace&) {}
    void on_trade(const Trade&) {}
    void on_directory(const StockDirectory&) {}
    void on_other(char) {}
};

inline Header header(const uint8_t* p) { return Header{char(p[0]), be16(p + 1), be48(p + 5)}; }

// Decode one message body (without the length prefix). `len` is the frame length.
template <class V>
Status decode(const uint8_t* p, size_t len, V& v) {
    if (len < 1) return Status::Truncated;
    const char t = char(p[0]);
    const size_t need = message_size(t);
    if (need == 0) return Status::UnknownType;
    if (len != need) return Status::BadLength;  // ITCH frames are exact; anything else is corrupt
    const Header h = header(p);
    const uint8_t* b = p + 11;
    switch (t) {
        case 'A': case 'F': {
            AddOrder m{h, be64(b), char(b[8]), be32(b + 9), {}, be32(b + 21)};
            std::memcpy(m.stock, b + 13, 8);
            v.on_add(m);
            break;
        }
        case 'E': v.on_executed(OrderExecuted{h, be64(b), be32(b + 8), be64(b + 12)}); break;
        case 'C': v.on_executed_price(OrderExecutedWithPrice{h, be64(b), be32(b + 8), be64(b + 12), be32(b + 21)}); break;
        case 'X': v.on_cancel(OrderCancel{h, be64(b), be32(b + 8)}); break;
        case 'D': v.on_delete(OrderDelete{h, be64(b)}); break;
        case 'U': v.on_replace(OrderReplace{h, be64(b), be64(b + 8), be32(b + 16), be32(b + 20)}); break;
        case 'P': {
            Trade m{h, be64(b), char(b[8]), be32(b + 9), {}, be32(b + 21), be64(b + 25)};
            std::memcpy(m.stock, b + 13, 8);
            v.on_trade(m);
            break;
        }
        case 'R': {
            StockDirectory m{h, {}};
            std::memcpy(m.stock, b, 8);
            v.on_directory(m);
            break;
        }
        default: v.on_other(t);
    }
    return Status::Ok;
}

// Walk a buffer of length-prefixed frames. Returns bytes consumed; stops at the first
// incomplete frame (so a caller streaming from a file can refill and resume) or error.
struct WalkResult { size_t consumed; size_t messages; Status status; };

template <class V>
WalkResult walk(const uint8_t* data, size_t n, V& v) {
    size_t off = 0, count = 0;
    while (off + 2 <= n) {
        const size_t len = be16(data + off);
        if (off + 2 + len > n) return {off, count, Status::Truncated};
        const Status s = decode(data + off + 2, len, v);
        if (s != Status::Ok && s != Status::UnknownType) return {off, count, s};
        off += 2 + len;
        ++count;
    }
    return {off, count, off == n ? Status::Ok : Status::Truncated};
}

}  // namespace lob::itch
