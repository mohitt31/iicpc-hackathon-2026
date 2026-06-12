# Benchmark and Test Verification Results (Arch Linux Run)
Execution logs and performance metrics from running the latest upstream bot engine
on Arch Linux (16-core, pinning engaged, no isolcpus — a shared dev host).
---
## 1. Unit Tests (`test_order_book`)
All 19 matching-engine unit tests passed.
=== Results: 19/19 passed ===

(price-time priority, partial fills, IOC market orders, cancels, duplicate-seq replay
protection, maker-before-taker ordering, engine-seq monotonicity, full-run determinism)
## 2. Planted Bug Demo (`demo/run_demo.sh`)
The byte-exact validator caught the LIFO (price-time priority) bug. Both journals were
the same length (852 bytes); the diff pinpointed the exact divergent byte:
DIVERGE @ byte 244: A=0x01 B=0x05
A: message #7 (msg_type=4, engine_seq=7, order_seq=1) <- oldest order (correct, FIFO)
B: message #7 (msg_type=4, engine_seq=7, order_seq=5) <- newest order (the bug, LIFO)

## 3. Coordinated Omission (CO) Proof  (600k orders, 5ms stall every 20k, 100us interval)
Naive samples: 600,000 | CO-corrected: 653,584 (53,584 phantom backfilled). 600k/600k acked.
| Percentile | Naive (ns) | CO-Corrected (ns) | Ratio |
|---|---|---|---|
| p50    | 25,887    | 26,479    | 1.0x  |
| p90    | 80,895    | 141,823   | 1.8x  |
| p99    | 173,439   | 3,414,015 | 19.7x |
| p99.9  | 520,191   | 4,820,991 | 9.3x  |
| p99.99 | 5,189,631 | 5,251,071 | 1.0x  |
| Max    | 5,492,735 | 5,562,735 | 1.0x  |
## 4. Integration Test (`test/integration_test.sh`)  — PASS
Connected: 1/1 Sent=10000 Acked=10000 Fills=14954 Rejects=58 EAGAIN=646255
PartialAborts=0 PoolExhausted=0 PendingCollisions=0 -> PASS
Engine activity: 15012 (Fills + Rejects) INTEGRATION TEST: PASS

## 5. Live == Offline Replay (determinism)  (200k orders, 50us interval)
Naive samples: 200,000 | CO-corrected: 369,785 (169,785 phantom).
Byte-exact diff of the live server output journal vs the offline replay:
IDENTICAL: 25,743,624 bytes match
Diff exit code: 0

## 6. 32-Bot Scale Test  (8x oversubscription, 200us interval)
Sent=1,279,594 Acked=1,279,594 PartialAborts=0 Collisions=0
EAGAIN=20,306,947 (backpressure; no lost or double-counted messages)

Threads 16-32 ran without core pinning (16-core host); the system handled the
scheduling pressure gracefully with zero failed sends or partial aborts.
---
## Cross-platform status
Determinism claims (19/19 unit tests, byte-244 diff, live==replay byte-exact) are
platform-independent and reproduce on Arch Linux and macOS. A WSL2/Windows run is
pending; its numbers will be appended to this file when available — no other
documentation change required.
