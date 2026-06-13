# IICPC HFT Benchmarking Platform

**Submission for IICPC Summer Hackathon 2026 - multi-track HFT engine benchmarking platform.**

The platform lets independent teams submit a C++ matching engine, runs it inside a sandbox, hammers it with a low-latency synthetic order flow, and ranks submissions by a composite of correctness, throughput, and tail latency.

The platform has three pieces: a low-latency C++ bot fleet that generates load, a sandbox that runs contestant matching engines under controlled isolation, and a realtime leaderboard that ranks submissions by composite score. The shared wire contract is in `contracts/`.

![AWS deployment architecture](docs/architecture_diagram.png)

---

## What's distinctive about this submission

Most teams will build "broad and fast" - a working pipeline, a leaderboard, some numbers on the page. This submission picks one thing that most benchmarks get wrong and makes it the centrepiece: **Coordinated Omission**.

A naive load generator records the time between sending an order and receiving its ack. If the engine stalls for 5 ms, the bot also stalls (it can't send more orders), so the *next* sample is recorded only after the stall ends - and shows up at the engine's normal latency, not 5 ms. The naive histogram says "p99 = 30 µs" while the real worst-case experience was 5 ms. This single bug invalidates almost every public latency benchmark.

The bot in this repository fixes this with the structural Tene/Snyder correction:

- Records latencies against **intended send time**, not actual send time
- Backfills phantom samples during stalls via `hdr_record_corrected_value`
- Pins SO_SNDBUF small so backpressure is visible, not hidden in kernel buffers

On a deterministic 5 ms stall every 20,000 orders, the measurement is **naive p99 173 µs vs CO-corrected p99 3.41 ms - a 19.7x gap** tabulated on a shared host, and up to 76x on isolated hardware (the cleaner the host, the harder a naive benchmark lies). Every contestant on this platform is measured the honest way.

A second distinctive choice: a **byte-exact correctness validator**. The platform ships a gold-standard reference matching engine. Every contestant's output journal is diffed against the reference's output on the same input. The included demo plants one common HFT bug (price-time priority violation - newest order matches first instead of oldest) into a buggy engine and shows the validator pinpointing the divergence on the first aggressive trade. No heuristics, no thresholds.

---

## Headline numbers (verified)

All numbers below were produced by the bot in this repository against `null_responder` and `reference_engine`. Logs and CSV outputs are under `bot-engine/verified_runs/` and `bot-engine/integration_results/`.

**Soak tests & Data Integrity:**
Across all committed runs (600k CO proof + 200k replay + 1.28M in the 32-bot test = 2M+ orders), zero integrity-counter violations (0 collisions, 0 pool exhaustion, 0 partial aborts). The full soak suite (soak/soak_test.sh) is reproducible.

**Coordinated Omission proof (deterministic 5 ms stall every 20 k orders):**

| Percentile | Naive (ns) | CO-Corrected (ns) | Ratio |
|---|---|---|---|
| p50    | 25,887    | 26,479    | 1.0x  |
| p90    | 80,895    | 141,823   | 1.8x  |
| p99    | 173,439   | 3,414,015 | 19.7x |
| p99.9  | 520,191   | 4,820,991 | 9.3x  |
| p99.99 | 5,189,631 | 5,251,071 | 1.0x  |

Run: 600,000 orders, 5 ms stall every 20,000, 100µs interval, Arch Linux (16-core,
no isolcpus). 600,000/600,000 acked, 0 violations, 53,584 phantom samples backfilled.

Reproduced independently on Arch Linux (pinning engaged) and macOS (pinning is a
no-op): the CO-corrected p99 stays ~3.4–5.1 ms on every host because it captures the
injected 5 ms stall; the naive-vs-CO ratio (19.7x shared host, up to 76x isolcpus)
tracks each host's baseline jitter. Tuned bare-metal (isolcpus + nohz_full +
SO_BUSY_POLL) measures p99 7.7µs (`verified_runs/aftab/baremetal_latency.txt`).

**Clean run, 4 bots, 100 µs interval (measured on an otherwise-idle machine; localhost numbers vary with background load):**

| Percentile | Naive (ns) | CO-corrected (ns) |
|---|---|---|
| p50 | 23,551 | 23,551 |
| p90 | 47,903 | 47,903 |
| p99 | 83,711 | 83,967 |
| Max | 300,799 | 300,799 |

Proof that this is environment, not engine: On a quiet, otherwise-idle machine, naive and CO-corrected p99 agree within ~1.1x. 
In our shared CI container, even 'clean' runs show inflated CO tails - that is the 
methodology correctly reporting real scheduler stalls, not a bug.

---

### Where these numbers come from (and the macOS caveat)

All headline numbers were measured in a deliberately hostile, un-isolated environment 
(shared cloud Linux container; on macOS the Linux-only pthread_setaffinity_np and 
SO_BUSY_POLL are no-ops). On such a host you are seeing scheduler jitter, not the 
engine - which is why the CO-corrected p99 is the honest reading. The engine's designed 
operating point is an isolated bare-metal Linux node (isolcpus + nohz_full + rcu_nocbs
+ SO_BUSY_POLL); bare-metal latency is **measured** at p99 7.7µs on an isolated
consumer desktop (i7-13620H) - `verified_runs/aftab/baremetal_latency.txt`.

---

## Quick start

Build everything and run the canonical demo:

```bash
# Dependencies - see BUILD.md for full install instructions
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

- **`bot-engine/src/bot.cpp`** - multi-threaded open-loop load generator with per-thread memory pools, CPU pinning, HDR histograms (naive + CO-corrected), per-second CSV snapshots, integrity gate, and an opt-in SPSC lock-free ring for HDR offload.
- **`bot-engine/src/null_responder.cpp`** - minimal multi-connection TCP server that acks every order; used for bot perf testing and CO proof.
- **`bot-engine/src/reference_engine.cpp`** - offline single-threaded matching engine (price-time priority, FIFO, IOC market orders) that acts as the gold standard for correctness diffs.
- **`bot-engine/src/hdr_merge.cpp`** - merges per-bot CSV snapshots into a fleet-wide unified time-series and a per-bot ranking, using max-of-percentiles per the telemetry contract (never averages percentiles).
- **`bot-engine/demo/buggy_engine.cpp`** - copy of `reference_engine.cpp` with one planted price-time priority bug (LIFO matching), used by the live demo.
- **`contracts/interface_contract_v1.h`** - frozen wire contract: 5 message types, fixed-point prices, little-endian, x86_64. All messages share a common 16-byte header for diff alignment.

---

## Sandbox isolation

Contestant engines run inside a Firecracker microVM with a read-only rootfs and an
intake pipeline (static source scan, attestation, rootfs packing) plus an
orchestrator that provisions the VM, sets up networking, and runs the benchmark.
The intake source-scan is a lint heuristic (it rejects e.g. `fork`), not a security
boundary. The seccomp allowlist is **specified** (`pack_rootfs.sh` writes the
profile) but runtime enforcement is **in progress** — nothing installs the profile
yet, so live syscall filtering is not claimed. See
`sandbox/test/malicious/README.md` for the exact per-layer enforcement status.
The reference engine is also exposed as a TCP server for in-sandbox correctness
checks, while the offline reference-engine binary remains the gold standard for
byte-exact diffing.

---

## What we deliberately did **not** build (and why)

Some optimizations from the research documents would not have improved this submission's score at our scale, and would have introduced risk to the working pipeline. The Architecture Blueprint ([`ARCHITECTURE.md`](ARCHITECTURE.md), section "Deliberate deferrals") covers the full deferral list with citations; the short version:

- **SIMD AVX2 batch ingester** - payoff begins at 1M+ msg/sec receive. Current per-bot rate is 10k/sec. Skipped.
- **AF_XDP kernel bypass** - needed for sub-µs receive. Current p99 already at 84 µs on TCP + `SO_BUSY_POLL`. Skipped.
- **Aeron / Chronicle transport** - while Aeron handles many-to-one fan-in exceptionally well, the integration overhead for a hackathon outweighs the sub-microsecond transport gain when our latency is already bottlenecked by the host OS scheduler. Skipped.

These are documented as deliberate engineering judgment, not gaps.

---

## License & contributors

Hackathon submission, IICPC 2026.