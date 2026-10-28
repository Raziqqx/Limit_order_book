#!/usr/bin/env python3
"""Independent top-of-book cross-check for ONE symbol, written from the ITCH 5.0 spec.

Shares no code with the C++ replay. Prints the best bid/ask (price x aggregate size) at the
same exchange-time checkpoints the C++ tool uses, in the same CSV format, so the two outputs
can be diffed:

    python3 tools/itch_tob.py ~/itch-data/12302019.NASDAQ_ITCH50 AAPL 30 > py_aapl.csv
    ./build/itch_replay ~/itch-data/12302019.NASDAQ_ITCH50 --symbols AAPL --tob-csv cpp.csv
    diff <(grep AAPL cpp.csv) py_aapl.csv

It has to walk every message in the file (the length prefix is the only way to find message
boundaries), so it is slower than C++: a few minutes for a full day in CPython.
"""
import struct
import sys

CHUNK = 1 << 26


def fmt_price(p):
    return f"{p // 10000}.{p % 10000:04d}"


def fmt_time(ns):
    s = ns // 1_000_000_000
    return f"{s // 3600:02d}:{s // 60 % 60:02d}:{s % 60:02d}"


def main():
    path, symbol = sys.argv[1], sys.argv[2]
    interval = int(sys.argv[3]) * 60 * 1_000_000_000 if len(sys.argv) > 3 else 30 * 60 * 1_000_000_000
    sym8 = symbol.encode().ljust(8)

    target = None          # stock locate of the symbol (learned from the 'R' directory message)
    orders = {}            # ref -> [side, price, qty]
    levels = ({}, {})      # (bids, asks): price -> aggregate qty
    next_cp = 0
    last_ts = 0
    unpack_from = struct.unpack_from

    def top():
        b, a = levels
        if b:
            bp = max(b)
            bid = (bp, b[bp])
        else:
            bid = (0, 0)
        if a:
            ap = min(a)
            ask = (ap, a[ap])
        else:
            ask = (0, 0)
        return bid, ask

    def emit(ts):
        (bp, bq), (ap, aq) = top()
        print(f"{fmt_time(ts)},{symbol},{fmt_price(bp)},{bq},{fmt_price(ap)},{aq}", flush=True)

    def take(ref, n):
        o = orders.get(ref)
        if o is None:
            return
        side, price, qty = o
        n = min(n, qty)
        lv = levels[side]
        lv[price] -= n
        if lv[price] == 0:
            del lv[price]
        o[2] = qty - n
        if o[2] == 0:
            del orders[ref]

    def add(ref, side, price, qty):
        orders[ref] = [side, price, qty]
        lv = levels[side]
        lv[price] = lv.get(price, 0) + qty

    print("time,symbol,bid,bid_qty,ask,ask_qty")
    with open(path, "rb") as f:
        buf = b""
        while True:
            chunk = f.read(CHUNK)
            if not chunk:
                break
            buf = buf + chunk if buf else chunk
            pos, end = 0, len(buf)
            while end - pos >= 2:
                ln = (buf[pos] << 8) | buf[pos + 1]
                if end - pos < 2 + ln:
                    break
                m = pos + 2
                pos = m + ln
                ts = int.from_bytes(buf[m + 5:m + 11], "big")
                if ts >= next_cp:
                    if next_cp:
                        emit(ts)
                    next_cp = (ts // interval + 1) * interval
                last_ts = ts
                t = buf[m]
                if target is None:
                    if t == 0x52 and buf[m + 11:m + 19] == sym8:  # 'R'
                        target = (buf[m + 1] << 8) | buf[m + 2]
                    continue
                if ((buf[m + 1] << 8) | buf[m + 2]) != target:
                    continue
                if t == 0x41 or t == 0x46:  # 'A' / 'F'
                    ref, side, qty = unpack_from(">QcI", buf, m + 11)
                    price = unpack_from(">I", buf, m + 32)[0]
                    add(ref, 0 if side == b"B" else 1, price, qty)
                elif t == 0x45 or t == 0x43 or t == 0x58:  # 'E' / 'C' / 'X'
                    ref, n = unpack_from(">QI", buf, m + 11)
                    take(ref, n)
                elif t == 0x44:  # 'D'
                    take(unpack_from(">Q", buf, m + 11)[0], 1 << 40)
                elif t == 0x55:  # 'U'
                    old, new, qty, price = unpack_from(">QQII", buf, m + 11)
                    o = orders.get(old)
                    if o is not None:
                        side = o[0]
                        take(old, 1 << 40)
                        add(new, side, price, qty)
            buf = buf[pos:]
    emit(last_ts)


if __name__ == "__main__":
    main()
