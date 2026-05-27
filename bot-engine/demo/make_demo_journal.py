#!/usr/bin/env python3
"""
Demo input journal generator — produces a scenario that GUARANTEES
the buggy engine (LIFO matching) diverges from the reference engine
(FIFO matching).

The scenario:
  1. Several BUY limit orders rest at price 100 (build a queue)
  2. A single aggressive SELL @ 100 hits the book
  3. Reference engine matches the OLDEST resting buy first (price-time)
  4. Buggy engine matches the NEWEST resting buy first (LIFO bug)
  5. The resulting Fill.order_seq will differ → diff tool catches it

Output format: matches interface_contract_v1.h exactly.
"""
import struct, sys

# Constants from interface contract
MSG_NEWORDER = 1
SIDE_BUY     = 0
SIDE_SELL    = 1
ORDER_LIMIT  = 0

FRAME_FMT  = "<BBH"
NEW_FMT    = "<QQIBB2xqQ"

assert struct.calcsize(FRAME_FMT) == 4,  "FrameHeader size mismatch"
assert struct.calcsize(NEW_FMT)   == 40, "NewOrder size mismatch"


def frame(msg_type, payload):
    return struct.pack(FRAME_FMT, msg_type, 0, len(payload)) + payload


def new_order(seq, ts, sym, otype, side, price, qty):
    return frame(MSG_NEWORDER,
        struct.pack(NEW_FMT, seq, ts, sym, otype, side, price, qty))


def main(out_path):
    with open(out_path, "wb") as f:
        # ── PHASE 1: Build resting buy queue at price 100 ──────────────
        # 5 BUY orders, each qty=10, all at price 100.
        # Reference (FIFO) will match these in order: seq 1, 2, 3, 4, 5.
        # Buggy   (LIFO) will match these in order: seq 5, 4, 3, 2, 1.
        for seq in range(1, 6):
            f.write(new_order(seq=seq, ts=seq, sym=1,
                              otype=ORDER_LIMIT, side=SIDE_BUY,
                              price=100, qty=10))

        # ── PHASE 2: Aggressive SELL hits the queue ──────────────────────
        # Single SELL @ 100, qty=15 → fills 1.5 of the resting orders.
        # Reference: fills seq 1 fully (10), then seq 2 partially (5).
        # Buggy:     fills seq 5 fully (10), then seq 4 partially (5).
        # The Fill messages will reference DIFFERENT order_seqs.
        f.write(new_order(seq=6, ts=6, sym=1,
                          otype=ORDER_LIMIT, side=SIDE_SELL,
                          price=100, qty=15))

        # ── PHASE 3: Another aggressive SELL to consume more ─────────────
        # Another SELL @ 100, qty=20 → fills 2 more resting orders.
        # The divergence compounds.
        f.write(new_order(seq=7, ts=7, sym=1,
                          otype=ORDER_LIMIT, side=SIDE_SELL,
                          price=100, qty=20))

    print(f"Wrote 7 messages to {out_path}", file=sys.stderr)
    print("Scenario: 5 BUYs at price 100, then 2 SELLs that cross.", file=sys.stderr)
    print("Expected divergence: Fill.order_seq differs between engines.", file=sys.stderr)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "demo_input.jrn"
    main(out)
