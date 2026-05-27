# bot-engine — Role A (Low-Latency C++)

This subdirectory is the **Role A submission**: a low-latency synthetic order-flow generator (bot fleet), a reference matching engine, and the correctness validator that ranks contestants byte-exact against the reference.

For project-wide context, see the [repository root README](../README.md).

---

## Components

```
bot-engine/
├── src/
│   ├── bot.cpp              # The load generator — see "Bot" section below
│   ├── null_responder.cpp   # Minimal TCP server that acks everything
│   ├── reference_engine.cpp # Gold-standard offline matching engine
│   ├── hdr_merge.cpp        # CSV merge & ranking tool (per-bot → fleet)
│   ├── spsc_queue.h         # Lock-free SPSC ring (Vyukov pattern)
│   └── tsc_util.h           # rdtscp_ns() — invariant-TSC nanosecond clock
├── demo/
│   ├── buggy_engine.cpp     # Reference engine + planted price-time bug
│   ├── make_demo_journal.py # Input generator that triggers the bug
│   ├── run_demo.sh          # One-command end-to-end demo
│   └── README.md            # Pitch story + technical explanation
├── soak/
│   ├── soak_test.sh         # 9-minute production-confidence suite
│   └── README.md
├── test/
│   └── integration_test.sh  # Bot vs reference_engine smoke test
└── CMakeLists.txt
```

---

## Bot — what it does and why each piece is there

The bot is an **open-loop, multi-threaded, CPU-pinned** load generator. Every design choice traces to one of two requirements: (1) measure latency honestly under stalls, and (2) be cheap enough on the hot path that the measuring instrument is quieter than what it's measuring.

### Open-loop send + intended-time tagging (CO correction)

The hot send loop pre-computes the next intended send time and catches up when it falls behind. Each in-flight order is tagged with its *intended* send time (not wall-clock at the moment of `send()`). On ack receipt, latency is `now − intended`. This is the structural fix for Coordinated Omission: stalls show up as latency, not as missing samples.

A second histogram receives the same value via `hdr_record_corrected_value(latency, expected_interval_ns)`, which backfills the phantom samples a stall would otherwise omit. Both histograms are reported side-by-side so the gap is visible.

### Per-thread state, zero shared writes on hot path

Each bot thread owns its socket, `OrderPool` (1024 pre-allocated `NewOrder` slots), `xorshift64` RNG state, pending-map (1M slots), and HDR histograms. Workers never write to shared state during a run. The main thread merges histograms via `hdr_add()` at the end (HDR is provably additive, so this is exact, not approximate).

### CPU pinning

`pthread_setaffinity_np` pins each worker to a dedicated core (skip core 0 — handles most IRQs). Without pinning the OS scheduler ping-pongs the worker across cores, causing ~1 ms preemption jitter that pollutes the p99. With pinning the only stalls visible are real ones.

macOS lacks `pthread_setaffinity_np`; the bot detects this and logs a warning. Pinning is Linux-deployment only.

### `_mm_pause()` in the catch-up loop

Inside the inner `while (now >= next_send_time)` block, an `_mm_pause()` hint runs first. On x86 this emits the `PAUSE` instruction (~30-cycle hint) — it deprioritizes the thread on SMT pipelines and prevents the memory-order-violation flush penalty when the spin condition becomes false. Free on the hot path, recovered many times over by the avoided flush. Wrapped in a `HFT_PAUSE` macro with `yield` on ARM and a no-op on other archs.

### `SO_BUSY_POLL` (Linux)

`setsockopt(SOL_SOCKET, SO_BUSY_POLL, 50µs)` asks the kernel to spin briefly checking the NIC RX queue instead of waiting for IRQ wakeup. Documented 5–20 µs win on Linux production NICs. Gracefully `EPERM` and continues on systems without `CAP_NET_ADMIN`. Linux-only via `#ifdef`.

### `SO_TIMESTAMPING` (Linux)

Requests HW NIC timestamps (TX + RX hardware, plus SW fallback) for 4-way latency decomposition (bot software | net out | engine | net in). On loopback or NICs without HW support the kernel transparently falls back to software timestamps. Hot-loop cmsg parsing is deferred to production deployment with a PTP-synced NIC; the localhost demo uses `rdtscp_ns` which already has ns precision. Linux-only via `#ifdef`.

### Integrity gate

Before the scoring run, the bot runs a 1,000-order self-test against the target. If the bot's *own* p99 exceeds `--gate-p99-us` (default 1,000 µs), it flags the entire run with `INTEGRITY: FAILED`, prints a warning banner inside the aggregate latency table, and exits with code 1 — so a leaderboard can auto-filter unreliable submissions. A `--no-gate` flag disables it for development.

### Per-second HDR snapshots

When `--snapshot-dir <path>` is set, each bot spawns a cold thread that samples its HDR histograms every 1 s and writes a CSV row. The leaderboard (Track C) consumes these CSVs for the "p99 over time" timeline view. Snapshot reads are concurrent with hot-path writes — HDR's 64-bit counts are aligned and torn reads are statistically negligible at 1 Hz. We deliberately do **not** mutex the hot path.

### Warmup-orders exclusion

`--warmup-orders N` excludes the latency of the first N acks from the HDR histograms (throughput counters are not affected). This removes TCP slow-start, ARP resolution, page faults, and CPU cache-warming spikes from the p99. Standard practice in `wrk2` and similar benchmarks. Default is 0 (no exclusion).

### SPSC lock-free HDR offload (opt-in)

With `--use-spsc`, each bot writes `(latency, interval)` tuples to a lock-free SPSC ring (Vyukov-style; cache-line-aligned head/tail; relaxed/acquire/release memory ordering). A cold consumer thread pops and calls `hdr_record_value` / `hdr_record_corrected_value`. At our scale (10 k/sec/bot) the saving is microscopic (HDR record is ~50 ns vs. an inline write of ~2 ns). This is built as an alternate code path, not a replacement: at 1 M+ msg/sec the offload is critical, and demonstrating the architecture matters for the engineering blueprint.

### Disconnect & connect-timeout handling

`recv() == 0` (peer closed cleanly) is detected and logged — no more 100 %-CPU infinite spin when an engine dies. The initial `connect()` is bounded by a 3-second `SO_SNDTIMEO`/`SO_RCVTIMEO` so a blackholed IP can't hang the bot thread in-kernel forever. Both timeouts are cleared after connect succeeds so the non-blocking hot path is not affected.

---

## Reference engine

Single-threaded, single-writer, deterministic by construction. Price-time priority FIFO. Monotonic engine sequence and timestamp (engine timestamp = engine sequence) — so the same input journal always produces the same output journal, byte-exact. This is what the diff tool compares against.

Handles `NewOrder` (LIMIT and MARKET / IOC), `CancelOrder`. Six reject reason codes including `REJECT_MARKET_INSUFFICIENT_LIQUIDITY` for an unfilled MARKET (added in contract v1.1).

Maker fill is emitted before taker fill on every match — this is a contract invariant the diff tool depends on.

---

## Diff tool

`./build/refengine diff A.jrn B.jrn` walks both output journals frame-by-frame and reports the first divergent byte with a human-readable decode of both messages (msg_type, msg_len, common header fields, and message-specific fields). Used by the demo to surface the planted price-time priority bug at the first aggressive Fill (different `order_seq` matched first).

The demo proves the validator catches a real-world bug type — exchange production incidents of FIFO violation are well documented. See `demo/README.md` for the pitch story.

---

## CLI flags

| Flag | Default | What it does |
|---|---|---|
| `--bots N` | 4 | Number of bot threads |
| `--start-core K` | 1 | First CPU core to pin (skips core 0) |
| `--no-pin` | (off) | Disable CPU pinning |
| `--interval-us N` | 100 | Send interval per bot, µs |
| `--duration-sec N` | 10 | Scoring run duration, seconds |
| `--ip A.B.C.D` | 127.0.0.1 | Target engine IP |
| `--port N` | 9000 | Target engine port |
| `--snapshot-dir PATH` | (off) | Per-second HDR snapshots → CSV |
| `--gate-p99-us N` | 1000 | Integrity-gate self-test p99 limit (µs) |
| `--no-gate` | (off) | Skip the integrity-gate self-test |
| `--warmup-orders N` | 0 | Exclude first N order acks from HDR |
| `--use-spsc` | (off) | Route HDR records through lock-free ring |

Exit codes: **0** = pass; **1** = integrity gate failed (samples flagged); **2** = could not reach target.

---

## Running the canonical tests

```bash
# Build
mkdir -p build && cd build && cmake .. && make -j && cd ..

# CO proof — run with stall-mode responder
./build/null_responder --stall-mode --stall-every 20000 --stall-ms 5 &
./build/bot --bots 1 --interval-us 100 --duration-sec 30 --no-gate
kill %1

# Live correctness-validator demo
./demo/run_demo.sh

# Soak suite (~9 min)
./soak/soak_test.sh

# Integration smoke test (~12 sec)
./test/integration_test.sh
```

---

## Caveats — be honest

- **macOS pinning is a no-op** — `pthread_setaffinity_np` is Linux-only. Demo runs but with scheduler jitter.
- **`SO_BUSY_POLL` requires `CAP_NET_ADMIN`** — without it the setsockopt fails with EPERM and is silently skipped.
- **`SO_TIMESTAMPING` is request-only here** — full cmsg parse is in the production roadmap.
- **The integration-test wrapper is Python** — it's a correctness check, not a perf check. HDR numbers from it should be ignored.
- **The bot is open-source-clean C++** — no AVX-512 intrinsics, no AF_XDP, no Aeron. Those would not improve the score at this scale and are documented in the Architecture Blueprint as deliberate deferrals.
