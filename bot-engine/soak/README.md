# Soak Test Suite

Production-confidence tests for the bot fleet. Three tests, ~9 minutes
total, generate a Markdown summary with headline numbers for the
Architecture Blueprint and pitch.

## What this proves

| Claim                                    | Test that proves it     |
|------------------------------------------|-------------------------|
| Zero malloc on hot path                  | Test 2 (leak detector)  |
| Memory pool never exhausts under load    | Tests 1 & 3 (counters)  |
| Throughput is stable over time           | Test 1 (5-min soak)     |
| Multi-bot scales without contention      | Test 3 (4-bot stress)   |
| TCP wire integrity at sustained rate     | Tests 1 & 3 (counters)  |
| Pending map sized correctly (1M slots)   | Tests 1 & 3 (counters)  |

## How to run

From `bot-engine/`:

```bash
# One-time build
cd build && cmake .. && make -j
cd ..

# Run the soak suite
chmod +x soak/soak_test.sh
./soak/soak_test.sh
```

Results go to `soak_results/<timestamp>/`. The `SUMMARY.md` in that
directory has the pass/fail verdict for each test plus headline numbers.

## What "PASS" means per test

**Test 1 (5-min soak):**
- `PoolExhausted == 0` — memory pool sized correctly
- `PendingCollisions == 0` — pending map sized correctly
- `PartialAborts == 0` — TCP wire integrity
- `Acked == Sent` — no lost responses

**Test 2 (leak check):**
- valgrind: `definitely lost: 0 bytes`
- macOS leaks: `0 leaks for 0 total leaked bytes`
- Either tool reporting clean = pool design verified

**Test 3 (multi-bot stress):**
- Same counters as Test 1, all zero
- Throughput on the same order of magnitude as Test 1 × 4

## OS compatibility

| OS     | Leak tool used     | Notes                              |
|--------|--------------------|------------------------------------|
| Linux  | valgrind           | Most accurate. Slow (10x runtime). |
| macOS  | `leaks` (built-in) | Snapshots while bot is running.    |
| Other  | None — test 2 skipped | Tests 1 & 3 still run.          |

On macOS, `MallocStackLogging=1` is enabled automatically so `leaks`
output is accurate. Without it, `leaks` may miss small allocations.

## Files in this directory

| File              | Purpose                                |
|-------------------|----------------------------------------|
| `soak_test.sh`    | Main test runner                       |
| `README.md`       | This file                              |
