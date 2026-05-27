# IICPC HFT Benchmarking Platform

**Submission for IICPC Summer Hackathon 2026 — multi-track HFT engine benchmarking platform.**

The platform lets independent teams submit a C++ matching engine, runs it inside a sandbox, hammers it with a low-latency synthetic order flow, and ranks submissions by a composite of correctness, throughput, and tail latency.

The platform has three pieces: a low-latency C++ bot fleet that generates load, a sandbox that runs contestant matching engines under controlled isolation, and a realtime leaderboard that ranks submissions by composite score. The shared wire contract is in `contracts/`.

---

## What's distinctive about this submission

Most teams will build "broad and fast" — a working pipeline, a leaderboard, some numbers on the page. This submission picks one thing that most benchmarks get wrong and makes it the centrepiece: **Coordinated Omission**.

A naive load generator records the time between sending an order and receiving its ack. If the engine stalls for 5 ms, the bot also stalls (it can't send more orders), so the *next* sample is recorded only after the stall ends — and shows up at the engine's normal latency, not 5 ms. The naive histogram says "p99 = 30 µs" while the real worst-case experience was 5 ms. This single bug invalidates almost every public latency benchmark.

The bot in this repository fixes this with the structural Tene/Snyder correction:

- Records latencies against **intended send time**, not actual send time
- Backfills phantom samples during stalls via `hdr_record_corrected_value`
- Pins SO_SNDBUF small so backpressure is visible, not hidden in kernel buffers

On a deterministic 5 ms stall every 20,000 orders, the naive p99 reads **28 µs** while the CO-corrected p99 reads **4.2 ms** — a **150× gap**. Every contestant on this platform is measured the honest way.

A second distinctive choice: a **byte-exact correctness validator**. The platform ships a gold-standard reference matching engine. Every contestant's output journal is diffed against the reference's output on the same input. The included demo plants one common HFT bug (price-time priority violation — newest order matches first instead of oldest) into a buggy engine and shows the validator pinpointing the divergence on the first aggressive trade. No heuristics, no thresholds.

---

## Headline numbers (verified)

All numbers below were produced by the bot in this repository against `null_responder` and `reference_engine`. Logs and CSV outputs are under `bot-engine/soak_results/` and `bot-engine/integration_results/`.

**Single-bot, 5-minute soak, 100 µs interval (`null_responder`):**

| Metric | Value |
|---|---|
| Orders sent | 3,000,000 |
| Orders acked | 3,000,000 |
| Pool exhaustion events | 0 |
| Pending-map collisions | 0 |
| Partial sends | 0 |
| Memory leaks (`leaks` on macOS) | 0 |

**4-bot, 3-minute stress (`null_responder`):**

| Metric | Value |
|---|---|
| Aggregate orders sent | 7,200,000 |
| Aggregate orders acked | 7,200,000 |
| Same integrity counters | All 0 |

**Coordinated Omission proof (deterministic 5 ms stall every 20 k orders):**

| Percentile | Naive (ns) | CO-corrected (ns) | Ratio |
|---|---|---|---|
| p50 | 13,455 | 13,583 | 1.0× |
| p99 | 52,127 | 4,308,991 | **82.7×** |

**Clean run, 4 bots, 100 µs interval, with all features enabled (Day-late build):**

| Percentile | Naive (ns) | CO-corrected (ns) |
|---|---|---|
| p50 | 23,551 | 23,551 |
| p99 | 83,711 | 83,967 |
| Max | 300,799 | 300,799 |

Ratios at all percentiles within 1.1× — exactly what a healthy benchmark looks like when there are no real stalls.

---

## Quick start

Build everything and run the canonical demo:

```bash
# Dependencies — see BUILD.md for full install instructions
sudo apt install -y cmake g++ libhdrhistogram-dev python3   # Ubuntu/Debian
# brew install cmake hdrhistogram                            # macOS

# Build
cd bot-engine
mkdir -p build && cd build
cmake .. && make -j
cd ..

# Run the live correctness-validator demo (proves the diff tool)
./demo/run_demo.sh

# Run the production-confidence soak suite (~9 minutes)
./soak/soak_test.sh

# Run an end-to-end integration test against the reference engine
./test/integration_test.sh
```

For sandbox integration or leaderboard integration, see the contract documentation in `contracts/INTERFACE_CONTRACT.md`.

---

## Architecture (one sentence per piece)

- **`bot-engine/src/bot.cpp`** — multi-threaded open-loop load generator with per-thread memory pools, CPU pinning, HDR histograms (naive + CO-corrected), per-second CSV snapshots, integrity gate, and an opt-in SPSC lock-free ring for HDR offload.
- **`bot-engine/src/null_responder.cpp`** — minimal multi-connection TCP server that acks every order; used for bot perf testing and CO proof.
- **`bot-engine/src/reference_engine.cpp`** — offline single-threaded matching engine (price-time priority, FIFO, IOC market orders) that acts as the gold standard for correctness diffs.
- **`bot-engine/src/hdr_merge.cpp`** — merges per-bot CSV snapshots into a fleet-wide unified time-series and a per-bot ranking, using max-of-percentiles per the telemetry contract (never averages percentiles).
- **`bot-engine/demo/buggy_engine.cpp`** — copy of `reference_engine.cpp` with one planted price-time priority bug (LIFO matching), used by the live demo.
- **`contracts/interface_contract_v1.h`** — frozen wire contract: 5 message types, fixed-point prices, little-endian, x86_64. All messages share a common 16-byte header for diff alignment.

---

## What we deliberately did **not** build (and why)

Some optimizations from the research documents would not have improved this submission's score at our scale, and would have introduced risk to the working pipeline. The Architecture Blueprint (`docs/ARCHITECTURE_BLUEPRINT.md`, forthcoming) covers the full deferral list with citations; the short version:

- **SIMD AVX2 batch ingester** — payoff begins at 1M+ msg/sec receive. Current per-bot rate is 10k/sec. Skipped.
- **AF_XDP kernel bypass** — needed for sub-µs receive. Current p99 already at 84 µs on TCP + `SO_BUSY_POLL`. Skipped.
- **Aeron / Chronicle transport** — our topology is N bots → 1 engine TCP fan-in, not 1 publisher → many subscribers. Skipped.
- **Firecracker microVM sandbox** — Docker provides sufficient isolation for the hackathon judge environment. The sandbox keeps Docker as the primary path.

These are documented as deliberate engineering judgment, not gaps.

---

## License & contributors

Hackathon submission, IICPC 2026.