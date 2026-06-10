#!/usr/bin/env python3
"""
Produces a mixed buy/sell input journal where at least 30% of orders cross.

Strategy (three waves):
  Wave 1 — 100 matched pairs: BUY LIMIT@100 immediately followed by SELL LIMIT@100.
            Each SELL crosses the BUY placed one message earlier (same price, same qty).
            Designed crossing rate from this wave alone: 100/200 = 50%.

  Wave 2 — 50 non-crossing orders to build resting depth:
            25 × BUY@99 and 25 × SELL@101 (spread too wide to cross each other).

  Wave 3 — 40 aggressive sweeps into that depth:
            20 × SELL@99 (crosses the resting BUY@99 from wave 2)
            20 × BUY@101 (crosses the resting SELL@101 from wave 2)

Total:       290 orders
Designed crosses: 100 + 20 + 20 = 140  →  48.3% > 30% threshold

Output format: identical to make_test_journal.py (wire format per interface_contract_v1.h).
"""
import struct
import sys

MSG_NEWORDER = 1
SIDE_BUY     = 0
SIDE_SELL    = 1
ORDER_LIMIT  = 0

FRAME_FMT = "<BBH"
NEW_FMT   = "<QQIBB2xqQ"

assert struct.calcsize(FRAME_FMT) == 4,  "FrameHeader size mismatch"
assert struct.calcsize(NEW_FMT)   == 40, "NewOrder size mismatch"


def new_order_bytes(seq, ts, sym, side, price, qty):
    payload = struct.pack(NEW_FMT, seq, ts, sym, ORDER_LIMIT, side, price, qty)
    header  = struct.pack(FRAME_FMT, MSG_NEWORDER, 0, len(payload))
    return header + payload


def main(out_path):
    seq = 0
    ts  = 1
    total    = 0
    crossing = 0

    with open(out_path, "wb") as f:

        # ── Wave 1: 100 pairs at the same price ──────────────────
        # BUY@100 rests for exactly one message before SELL@100 arrives
        # and crosses it.  Each pair produces 2 orders, 1 of which crosses.
        for _ in range(100):
            seq += 1; ts += 1
            f.write(new_order_bytes(seq, ts, 1, SIDE_BUY,  100, 1))
            total += 1

            seq += 1; ts += 1
            f.write(new_order_bytes(seq, ts, 1, SIDE_SELL, 100, 1))
            total    += 1
            crossing += 1   # this SELL crosses the just-placed BUY

        # ── Wave 2: resting depth (no crosses) ───────────────────
        # BUY@99 and SELL@101 are 2 ticks apart — they will never match
        # each other but create depth for wave 3 to sweep.
        for _ in range(25):
            seq += 1; ts += 1
            f.write(new_order_bytes(seq, ts, 1, SIDE_BUY,  99,  2))
            total += 1

            seq += 1; ts += 1
            f.write(new_order_bytes(seq, ts, 1, SIDE_SELL, 101, 2))
            total += 1

        # ── Wave 3: aggressive sweeps into the resting depth ─────
        # SELL@99 limit price <= best bid (99) → crosses immediately.
        # BUY@101 limit price >= best ask (101) → crosses immediately.
        for _ in range(20):
            seq += 1; ts += 1
            f.write(new_order_bytes(seq, ts, 1, SIDE_SELL, 99,  2))
            total    += 1
            crossing += 1

            seq += 1; ts += 1
            f.write(new_order_bytes(seq, ts, 1, SIDE_BUY,  101, 2))
            total    += 1
            crossing += 1

    pct = 100.0 * crossing / total
    print(f"wrote {total} orders ({seq} messages) to {out_path}", file=sys.stderr)
    print(f"designed crossing orders: {crossing}/{total} = {pct:.1f}%", file=sys.stderr)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "crossing_input.jrn"
    main(out)
