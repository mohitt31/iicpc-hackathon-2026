# Demo: Correctness Validator vs Planted Bug

This directory contains a **live, reproducible demo** that proves our
byte-exact correctness validator actually catches subtle matching-engine
bugs.

## The story (for the pitch)

> *We built a reference matching engine — the gold standard for what
> correct behavior looks like. Then we built a 'contestant' engine with
> a single, very specific bug planted in it: it violates price-time
> priority. Same engine in every other way. Same wire protocol. Both
> engines ack every order. Both produce sensible-looking fills. Both
> have the same byte count in their output journals.*
>
> *Our diff tool catches the bug in milliseconds.*

## The planted bug

In `buggy_engine.cpp`, the `match_aggressive()` function walks the
price-level queue from the **back** instead of the **front** — newest
order matches first instead of oldest. This violates the foundational
FIFO rule of price-time priority.

Real exchanges (NYSE, NASDAQ, BATS) have had production bugs of exactly
this shape. The cost of detecting them late runs into millions.

## How to run

From the `bot-engine/` directory:

```bash
# One-time build
cd build && cmake .. && make -j buggy_engine
cd ..

# Run the demo
./demo/run_demo.sh
```

## What the demo proves

1. **Both engines compile, run, and produce same-sized output.** No
   crashes, no obvious errors. A naive "does it run?" test passes.
2. **The diff tool surfaces the exact divergent Fill message** —
   showing which `order_seq` was matched first, and which should
   have been matched first.
3. **The validation is byte-exact, not heuristic.** No threshold to
   tune, no false positives.

## Why this matters for IICPC

Every contestant submission will be diffed against the reference engine.
A submission that fails to follow price-time priority — even on one
trade out of a million — gets caught. The scoreboard reflects real
correctness, not "looks fine."

## Files

| File                   | Purpose                                       |
| ---------------------- | --------------------------------------------- |
| `buggy_engine.cpp`     | Reference engine + one planted LIFO bug       |
| `make_demo_journal.py` | Generates input that triggers the bug         |
| `run_demo.sh`          | End-to-end runner; produces the diff output   |
| `README.md`            | This file                                     |
