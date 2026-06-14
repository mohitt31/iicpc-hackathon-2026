# Architecture Blueprint — IICPC HFT Benchmarking Platform

A platform that lets independent teams submit a matching engine, runs each
submission under controlled isolation, hammers it with a low-latency synthetic
order flow, and ranks submissions by a composite of **correctness, throughput,
and tail latency** — measured honestly.

> One design choice runs through every layer: **measure the truth, then only
> display it.** The hard part of a latency benchmark is not drawing the chart —
> it is not lying in the chart. See *Coordinated Omission* below.

![AWS deployment architecture](docs/architecture_diagram.png)

---

## System at a glance

```
          submit engine                  load + telemetry                 display
        ┌───────────────┐   contract   ┌──────────────────┐   contract  ┌──────────────┐
        │   SANDBOX     │◀────────────▶│    BOT FLEET     │────────────▶│  LEADERBOARD │
        │ (Firecracker) │   SBE wire   │  + reference     │  scored      │  (browser,   │
        │  isolates the │   protocol   │    engine        │  deltas      │ display-only)│
        │  contestant   │              │  + HDR telemetry │  (WS/SSE)    │              │
        └───────────────┘              └──────────────────┘             └──────────────┘
              T1                              T2                               T3
                         all three build against contracts/interface_contract_v1.h  (FROZEN)
```

Three independently-built components, one frozen wire contract. A single field
change breaks three codebases — so the contract is frozen and versioned (v1.1).

---

## The contract (`contracts/`)

The seam that lets three people build in parallel. `interface_contract_v1.h` is
the frozen, little-endian, x86-64 wire format: a 4-byte `FrameHeader` followed
by one of five message types (`NewOrder`, `CancelOrder`, `OrderAck`, `Fill`,
`Reject`). Fixed-point prices (int64 ticks, never float). `seq` is the join key
that ties an ack back to the order that caused it; latency is
`ack_rx − NewOrder.timestamp_ns` for that `seq`.

`INTERFACE_CONTRACT.md` is the human-readable spec; the `.h` is what compiles
into all three codebases.

---

## Component 1 — Sandbox (`sandbox/`)

Runs each contestant's matching engine inside a Firecracker microVM with a
read-only rootfs, so a submission can be benchmarked without trusting it. The
isolation is **defence-in-depth, enforced and proven**: the read-only rootfs
and VM boundary are real, and the seccomp boundary is **enforced and proven** — a static PID-1 installs a default-KILL BPF filter before exec; 3 malicious fixtures were KILLED (SIGSYS) in a real microVM while a good submission survives (`verified_runs/aftab/firecracker_*`). The intake source-scan is a lint heuristic,
not a security boundary. `sandbox/test/malicious/README.md` documents the exact
per-layer status.

Pipeline (`pipeline.sh` orchestrates these stages):

- **intake** — accepts a submission (ELF/tarball), hashes it to a 128-bit
  `submission_id`, validates structure.
- **packer** — builds a minimal rootfs containing the submission.
- **builder** — compiles/links inside the controlled environment.
- **attester** — records a measurement of what is about to run (the integrity
  half of "did we benchmark what they submitted").
- **orchestrator** — provisions a Firecracker microVM, sets up networking, runs
  the benchmark, tears down.

The reference matching engine is also exposed as a TCP server for in-sandbox
correctness checks, while the offline reference binary remains the gold standard
for byte-exact diffing.

> Why Firecracker, not containers: a benchmark must isolate the *latency* of the
> submission, not just its filesystem. MicroVMs give a clean scheduling and
> kernel boundary; containers share the host scheduler and pollute the tail.

---

## Component 2 — Bot Fleet + Telemetry (`bot-engine/`)

An open-loop, multi-threaded, CPU-pinned load generator, a gold-standard
reference engine, and a byte-exact correctness validator. Full design notes live
in `bot-engine/`'s own README; the load-bearing ideas:

### Coordinated Omission correction (the centrepiece)

A naive load generator records the time between sending an order and receiving
its ack. If the engine stalls for 5 ms, the bot also stalls (it can't send more
orders), so the *next* sample is recorded only after the stall ends — and shows
up at normal latency, not 5 ms. The naive histogram reports "p99 = 30 µs" while
the real worst case was 5 ms. This single bug invalidates most public latency
benchmarks.

The fix is structural:

- Each in-flight order is tagged with its **intended** send time, not wall-clock
  at `send()`. On ack, latency is `now − intended`.
- A second histogram receives the same value via
  `hdr_record_corrected_value(latency, expected_interval)`, backfilling the
  phantom samples a stall would otherwise omit.
- `SO_SNDBUF` is pinned small so backpressure is visible, not hidden in kernel
  buffers.

Both histograms are reported side-by-side so the gap is provable. On a
deterministic 5 ms stall every 20 k orders, naive p99 reads 173 µs while
CO-corrected p99 reads 3.41 ms — a 19.7x gap tabulated on a shared host, up to 76x on
isolated hardware (the cleaner the host, the harder a naive benchmark lies). Every contestant is measured the
honest way.

### Honest measurement under load

CPU pinning (skip core 0), `_mm_pause()` in the catch-up spin, per-thread state
with zero shared writes on the hot path, additive HDR merge at the end (HDR is
provably additive — exact, not approximate). A **self-test integrity gate**: if
the bot's own p99 exceeds a threshold before the scoring run, the run is flagged
and the leaderboard auto-filters it — speed of the *measuring instrument* must
beat what it measures.

### Correctness validator

A deterministic reference engine (price-time priority FIFO, monotonic engine
sequence) produces a byte-exact output journal for any input. `refengine diff`
walks two journals and reports the first divergent byte with a decoded message.
The demo plants a price-time-priority bug (LIFO matching) and shows the
validator pinpointing it on the first aggressive fill.

---

## Component 3 — Real-Time Leaderboard (`frontend/`)

A single self-contained page that ranks submissions live. **Cardinal rule:
display only.** It never recomputes a percentile — no averaging, no
re-bucketing, no interpolation. It renders the contract's already-CO-corrected gauges. Within a single bot, histograms are merged with hdr_add() — exact and additive. Across bots and over time, hdr_merge ranks by max-of-percentiles (conservative); we never average percentiles.

Features that map directly to the spec:

- **Per-protocol boards** (BINARY (SBE v1) / WebSocket / REST), never mixed; an *Overall*
  view exists but is gated behind a warning because cross-protocol comparison is
  meaningless (TLS + JSON framing alone add tens of µs).
- **Full percentile ladder** p50 → p90 → p99 → p99.9 → p99.99 → max, in
  nanoseconds. No mean-latency headline anywhere.
- **Integrity gate** surfaced: submissions whose self-test p99 (>5 µs) or
  software-jitter (>1 µs) failed are *excluded* before ranking — a filter, not a
  penalty.
- **Single-clock RTT** per sample (`now - intended_send`, measured on the bot):
  no cross-machine clock sync, so there is nothing to drift or mis-configure.
  HW/PTP timestamping is a production decomposition step, not required here.
- **Composite score** `0.40·latency + 0.30·throughput + 0.30·correctness`, with
  a hard-fail cap: a correctness failure caps the total — speed never buys back
  incorrectness.
- **Delta streaming**: the gateway diffs each tick and the UI animates only what
  changed.

### Mock-first, zero-change live swap

The UI runs against a synthetic source by default. Setting `USE_LIVE=true` makes
it subscribe to a WebSocket emitting display-ready `{type:"deltas", deltas:{…}}`
frames; the page renders them directly. If the socket fails it silently falls
back to mock, so a demo is never blank. The reference gateway that emits this
feed is `tools/telemetry_server.js`; the production gateway (reading
VictoriaMetrics + additive HDR) is a drop-in replacement speaking the same JSON.

---

## Data flow, end to end

1. Contestant submits an engine → **sandbox** isolates and runs it, exposing it
   on the SBE wire protocol.
2. **Bot fleet** drives constant-arrival-rate load at it, tagging intended send
   times, recording CO-corrected HDR histograms, running the integrity self-test.
3. Per-second HDR snapshots + correctness diffs flow to the **telemetry gateway**,
   which scores and ranks per Interface Contract §5.
4. The gateway fans out **display-ready deltas** over WebSocket.
5. The **leaderboard** renders them — and only renders them.

---

## Deployment

- **Local / demo (`infra/docker-compose.yml`)** — one command brings up the
  reference engine, bot fleet, telemetry gateway, and leaderboard. Correctness
  and demo only; containers can't produce real tail-latency numbers.
- **Production (`infra/k8s/`, `infra/terraform/`)** — the bot fleet runs as a
  bare-metal `DaemonSet` with Guaranteed QoS and pinned cores; benchmark nodes
  are provisioned with `isolcpus`/`nohz_full` via Terraform. This is where the
  Linux-only latency features (CPU pinning, `SO_BUSY_POLL`, HW timestamping)
  actually engage.

---

## Deliberate deferrals (honest scope)

Documented here so they read as decisions, not gaps:

- **No AVX-512 / AF_XDP / Aeron** in the bot — at this scale they don't change
  the score and add portability cost; the SPSC offload path is built to show the
  architecture scales to 1 M+ msg/sec.
- **HW NIC timestamping is request-only** in the demo; full `cmsg` parse is a
  production-deployment step needing a PTP-synced NIC.
- **macOS CPU pinning is a no-op** (`pthread_setaffinity_np` is Linux-only); the
  bot detects this and degrades gracefully.
- **Compose path is not low-latency** by design; the K8s/bare-metal path is.

---

## Considered alternatives (and why not)

The brief's vocabulary, addressed directly so the choices read as decisions:

- **gRPC** — rejected *on the measured wire*. HTTP/2 + protobuf encode/decode would
  add tens of microseconds to the exact hot path we benchmark. The bot↔engine
  contract is a fixed-layout binary (SBE-style), parsed by pointer-cast, zero-copy.
  gRPC is appropriate for the *control plane*; the intake API is deliberately plain
  HTTP so it stays `curl`-able.
- **Kafka / Redpanda** — not on the telemetry path. HDR histograms are *additively
  mergeable*, so each bot ships a small pre-aggregated histogram blob cold rather
  than an event firehose. A log bus would add a network hop plus serialization for
  durable replay we don't need at benchmark timescale. Redpanda would only earn its
  place if we required durable replay of every individual order at 1 M+/s — the bot
  owns its own histogram, so we don't.
- **VictoriaMetrics over TimescaleDB / InfluxDB** — for the production metrics store,
  VictoriaMetrics aggregates high-cardinality histogram series where TimescaleDB and
  Prometheus struggle, at a fraction of the RAM. (Current demo path is CSV snapshots
  → gateway; VictoriaMetrics is the documented scale-out target.)
- **Redis** — *used*, not merely discussed: it backs the submission work-queue in
  `platform/intake-api/` (control plane), never the latency hot path.
