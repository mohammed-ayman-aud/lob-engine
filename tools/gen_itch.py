#!/usr/bin/env python3
"""Generate a synthetic but spec-exact NASDAQ TotalView-ITCH 5.0 file.

Same framing as NASDAQ's historical files ([u16 big-endian length][message]...), same
message layouts, plausible order flow: a random-walk mid price per stock, adds clustered
near the inside, and cancels, deletes, partial executions and replaces at realistic ratios.
Used for tests, benchmarks and the fuzzing seed corpus. The real thing is in
tools/run_real_data.sh.

usage: gen_itch.py OUT --messages 5000000 --stocks 100 --seed 1
"""
import argparse
import random
import struct

TICK = 100  # $0.01 in ITCH price units (1e-4 dollars)


def header(t, locate, ts):
    # type(1) locate(2) tracking(2) timestamp(6)
    return struct.pack(">cHH", t, locate, 0) + ts.to_bytes(6, "big")


class Writer:
    def __init__(self, f):
        self.f, self.n, self.ts = f, 0, 34_200_000_000_000  # 09:30:00 in ns since midnight

    def emit(self, body):
        self.f.write(struct.pack(">H", len(body)) + body)
        self.n += 1

    def tick(self, rng):
        self.ts += rng.randint(50, 5000)
        return self.ts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--messages", type=int, default=1_000_000)
    ap.add_argument("--stocks", type=int, default=100)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    rng = random.Random(a.seed)

    with open(a.out, "wb") as f:
        w = Writer(f)
        w.emit(header(b"S", 0, w.tick(rng)) + b"O")                     # start of messages
        names = [f"SYM{i:04d}".encode().ljust(8) for i in range(a.stocks)]
        for i, name in enumerate(names, start=1):                         # stock directory 'R'
            body = header(b"R", i, w.tick(rng)) + name + b"Q" + b"N" + struct.pack(">I", 100) + b"N" + b"C" + b"  " + b"P" + b"N" + b"N" + b"1" + b"N" + struct.pack(">I", 0) + b"N"
            assert len(body) == 39
            w.emit(body)

        # activity is skewed: a few names trade a lot (Zipf-like weights)
        weights = [1.0 / (k + 1) ** 0.9 for k in range(a.stocks)]
        mids = [rng.randint(10, 500) * TICK * 10 for _ in range(a.stocks)]
        live = [dict() for _ in range(a.stocks)]   # ref -> [side, price, shares]
        keys = [list() for _ in range(a.stocks)]   # refs, for O(1) random choice (lazy deletion)
        next_ref, match = 1, 1

        def pick(s):
            ks = keys[s]
            while ks:
                i = rng.randrange(len(ks))
                ref = ks[i]
                if ref in live[s]:
                    return ref
                ks[i] = ks[-1]
                ks.pop()
            return None

        def new_price(s, side):
            off = 1 + int(rng.expovariate(0.35))             # ticks away from the inside
            return mids[s] - off * TICK if side == b"B" else mids[s] + off * TICK

        while w.n < a.messages:
            s = rng.choices(range(a.stocks), weights)[0]
            loc = s + 1
            if rng.random() < 0.02:
                mids[s] = max(TICK * 10, mids[s] + rng.choice((-TICK, TICK)))
            r = rng.random()
            ref = pick(s) if r >= 0.42 else None
            ts = w.tick(rng)
            if ref is None:                                   # Add Order ('A', or 'F' with MPID)
                side = rng.choice((b"B", b"S"))
                price, shares = new_price(s, side), rng.choice((100, 100, 100, 200, 300, 500, 1000, rng.randint(1, 99)))
                body = header(b"A", loc, ts) + struct.pack(">QcI", next_ref, side, shares) + names[s] + struct.pack(">I", price)
                if rng.random() < 0.05:
                    body = b"F" + body[1:] + b"GSCO"
                w.emit(body)
                live[s][next_ref] = [side, price, shares]
                keys[s].append(next_ref)
                next_ref += 1
            elif r < 0.72:                                    # Order Delete
                w.emit(header(b"D", loc, ts) + struct.pack(">Q", ref))
                del live[s][ref]
            elif r < 0.80:                                    # Order Cancel (partial)
                o = live[s][ref]
                q = rng.randint(1, o[2])
                w.emit(header(b"X", loc, ts) + struct.pack(">QI", ref, q))
                o[2] -= q
                if o[2] == 0:
                    del live[s][ref]
            elif r < 0.90:                                    # Order Executed (E or C)
                o = live[s][ref]
                q = rng.randint(1, o[2])
                if rng.random() < 0.1:
                    w.emit(header(b"C", loc, ts) + struct.pack(">QIQcI", ref, q, match, b"Y", o[1]))
                else:
                    w.emit(header(b"E", loc, ts) + struct.pack(">QIQ", ref, q, match))
                match += 1
                o[2] -= q
                if o[2] == 0:
                    del live[s][ref]
            elif r < 0.98:                                    # Order Replace
                o = live[s].pop(ref)
                price, shares = new_price(s, o[0]), rng.choice((100, 200, 300))
                w.emit(header(b"U", loc, ts) + struct.pack(">QQII", ref, next_ref, shares, price))
                live[s][next_ref] = [o[0], price, shares]
                keys[s].append(next_ref)
                next_ref += 1
            else:                                             # non-displayed Trade ('P'): no book change
                side = rng.choice((b"B", b"S"))
                w.emit(header(b"P", loc, ts) + struct.pack(">QcI", 0, side, 100) + names[s] + struct.pack(">IQ", mids[s], match))
                match += 1
        w.emit(header(b"S", 0, w.tick(rng)) + b"C")                     # end of messages
    print(f"wrote {w.n:,} messages to {a.out}")


if __name__ == "__main__":
    main()
