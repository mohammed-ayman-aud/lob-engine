#!/usr/bin/env python3
"""Reference order book in plain Python, for differential testing of the C++ engine.

Deliberately simple (dicts, no cleverness) so it is easy to trust. Replays the same ITCH
file and prints the top N levels of every book in the same format as `replay --dump`.
usage: ref_book.py FILE [--depth 5] > ref.txt
"""
import argparse
import struct
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--depth", type=int, default=5)
    a = ap.parse_args()
    data = open(a.file, "rb").read()
    orders = {}   # ref -> [locate, side, price, shares]
    books = {}    # (locate, side) -> {price: [shares, count]}
    symbols = {}

    def add(loc, ref, side, price, shares):
        orders[ref] = [loc, side, price, shares]
        lvl = books.setdefault((loc, side), {}).setdefault(price, [0, 0])
        lvl[0] += shares
        lvl[1] += 1

    def reduce(ref, q):
        o = orders.get(ref)
        if o is None:
            return
        q = min(q, o[3])
        lv = books[(o[0], o[1])]
        lv[o[2]][0] -= q
        o[3] -= q
        if o[3] == 0:
            lv[o[2]][1] -= 1
            if lv[o[2]][1] == 0:
                del lv[o[2]]
            del orders[ref]

    off, n = 0, len(data)
    while off + 2 <= n:
        ln = struct.unpack_from(">H", data, off)[0]
        m = data[off + 2: off + 2 + ln]
        off += 2 + ln
        t, loc = m[0:1], struct.unpack_from(">H", m, 1)[0]
        if t == b"R":
            symbols[loc] = m[11:19].decode().strip()
        elif t in (b"A", b"F"):
            ref, side, shares = struct.unpack_from(">QcI", m, 11)
            price = struct.unpack_from(">I", m, 32)[0]
            if ref not in orders:
                add(loc, ref, side.decode(), price, shares)
        elif t in (b"E", b"C"):
            ref, q = struct.unpack_from(">QI", m, 11)
            reduce(ref, q)
        elif t == b"X":
            ref, q = struct.unpack_from(">QI", m, 11)
            reduce(ref, q)
        elif t == b"D":
            ref = struct.unpack_from(">Q", m, 11)[0]
            if ref in orders:
                reduce(ref, orders[ref][3])
        elif t == b"U":
            old, new, shares, price = struct.unpack_from(">QQII", m, 11)
            if old in orders and new not in orders:
                loc0, side = orders[old][0], orders[old][1]
                reduce(old, orders[old][3])
                add(loc0, new, side, price, shares)

    out = sys.stdout
    for loc in sorted(symbols):
        for side in "BS":
            lv = books.get((loc, side), {})
            prices = sorted(lv, reverse=(side == "B"))[: a.depth]
            for p in prices:
                out.write(f"{symbols[loc]} {side} {p} {lv[p][0]} {lv[p][1]}\n")


if __name__ == "__main__":
    main()
