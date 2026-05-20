#!/usr/bin/env python3
"""
Deterministic input journal generator for the reference engine.
Same seed -> same bytes every run -> a true determinism test.

REPRODUCIBILITY: random.Random(seed) is documented stable across CPython
3.2+ for the methods used here (random(), randint(), choice()). Tested
against CPython 3.8-3.12. If you switch to PyPy or a different Python
implementation, regenerate the test fixtures and commit them.

Output format matches interface_contract_v1.h exactly:
  [FrameHeader 4B][Payload N bytes] ... sequentially.
"""
import struct, random, sys

SEED       = 42
NUM_ORDERS = 200

MSG_NEWORDER = 1
MSG_CANCEL   = 2
SIDE_BUY     = 0
SIDE_SELL    = 1
ORDER_LIMIT  = 0
ORDER_MARKET = 1

FRAME_FMT  = "<BBH"
NEW_FMT    = "<QQIBB2xqQ"
CANCEL_FMT = "<QQI4xQ"

assert struct.calcsize(FRAME_FMT)  == 4,  "FrameHeader size mismatch"
assert struct.calcsize(NEW_FMT)    == 40, "NewOrder size mismatch"
assert struct.calcsize(CANCEL_FMT) == 32, "CancelOrder size mismatch"

def frame(msg_type, payload):
    return struct.pack(FRAME_FMT, msg_type, 0, len(payload)) + payload

def new_order(seq, ts, sym, otype, side, price, qty):
    return frame(MSG_NEWORDER,
        struct.pack(NEW_FMT, seq, ts, sym, otype, side, price, qty))

def cancel(seq, ts, sym, target_seq):
    return frame(MSG_CANCEL,
        struct.pack(CANCEL_FMT, seq, ts, sym, target_seq))

def main(out_path):
    rng = random.Random(SEED)
    seq = 0
    ts  = 1
    live_orders = []

    with open(out_path, "wb") as f:
        for _ in range(NUM_ORDERS):
            seq += 1
            ts  += 1

            if rng.random() < 0.80 or not live_orders:
                side  = rng.choice([SIDE_BUY, SIDE_SELL])
                price = rng.randint(99, 101)
                qty   = rng.randint(1, 5)
                if rng.random() < 0.05:
                    f.write(new_order(seq, ts, 1, ORDER_MARKET, side, 0, qty))
                else:
                    f.write(new_order(seq, ts, 1, ORDER_LIMIT, side, price, qty))
                    live_orders.append(seq)
            else:
                target = rng.choice(live_orders)
                live_orders.remove(target)
                f.write(cancel(seq, ts, 1, target))

    print(f"wrote {seq} messages to {out_path}", file=sys.stderr)

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "test_input.jrn"
    main(out)
