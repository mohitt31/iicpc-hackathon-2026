# IICPC Summer Hackathon 2026: Design Document
## A Distributed Benchmarking & Hosting Platform for Trading Infrastructure

> **Thesis in one line:** The hard part of a latency benchmark is not drawing the
> chart - it is *not lying in the chart*. We built the entire platform around
> that single idea: **measure the truth, then only display it.**

---

## 0. How to read this document

This is both the **High-Level Design (HLD)** and the **Low-Level Design (LLD)**.
It is organised so a judge can grade it against the stated rubric in order:

| Rubric priority | Where it lives in this doc |
|---|---|
| 1. Engineering excellence - *how we understood & solved the problem* | §2 Problem Analysis, §4-§8 component LLD |
| 2. Product decisions | §3 Architecture Thesis, §9 Decision Log |
| 3. Multiple approaches & trade-offs | §9 Decision Log (every row = alternatives + why) |
| 4. End-to-end thinking | §3.3 Data Flow, §10 Deployment |
| 5. Deploy-ready code | §10 IaC, §11 Repro & verified tests |
| 6. Tests performed & verified | §11 Verified Results (every number reproducible) |

We optimised for **depth over breadth**: rather than four half-built components,
we built three that are *correct* and made the one thing most benchmarks get
wrong - **Coordinated Omission** - the centerpiece.

---

## 1. Executive Summary

We were asked to build a platform that lets contestants upload a matching
engine, hosts it under isolation, bombards it with a distributed bot fleet, and
ranks it live on latency, throughput, and correctness.

We built exactly that, as **three independently-developed components bound by one
frozen wire contract**:

1. **Sandbox** (`sandbox/`) - a Firecracker-microVM pipeline that takes an
 untrusted submission and runs it with a real scheduling/kernel boundary, so we
 benchmark the submission's *latency*, not just isolate its filesystem.
2. **Bot Fleet + Telemetry** (`bot-engine/`) - an open-loop, CPU-pinned, C++
 load generator that measures tail latency **honestly** (Coordinated-Omission
 corrected), plus a gold-standard reference engine and a **byte-exact**
 correctness validator.
3. **Real-Time Leaderboard** (`frontend/`) - a display-only board that streams
 live deltas and **never recomputes a percentile** - it can't lie because it
 only renders what the scoring service already proved.

The seam between them is `contracts/interface_contract_v1.h` - a **frozen,
versioned** binary wire format. One field change breaks three codebases, so we
froze it on day one. This is what let three people build in parallel without
integration hell.

**What makes this submission different:** most teams will build "broad and fast"
- a pipeline, a leaderboard, some numbers on a page. We picked the one bug that
invalidates *almost every public latency benchmark* - Coordinated Omission - and
made the honest measurement of it the spine of the whole system. Everything else
serves that truth.

---

## 2. Problem Analysis: what we actually understood

Most teams will read the prompt as "build 4 boxes: upload, sandbox, load, chart."
That reading loses. Here is what the problem *actually* demands, and where the
real engineering risk lives.

### 2.1 The benchmark's job is to be trustworthy, not pretty

A benchmarking platform produces *rankings that decide a winner*. If the numbers
are wrong, the platform is worse than useless - it confidently rewards the wrong
engine. So the dominant requirement is **measurement integrity**, not UI polish.
Three integrity threats dominate:

- **Coordinated Omission (CO).** A naive load generator sends an order, waits for
 the ack, records the round-trip, sends the next. If the engine stalls for 5 ms,
 the bot *also* stalls - it can't send during the stall - so the samples that
 *should* have landed during those 5 ms are never taken. The one sample after the
 stall records near-normal latency. The histogram reports **p99 = 30 µs** while
 the real worst-case experience was **5 ms**. This single bug is present in a
 huge fraction of public benchmarks. **A platform that ranks engines on CO-blind
 numbers ranks them wrong.**

- **The measuring instrument being slower than what it measures.** If the bot's
 own jitter is 1 ms, it cannot resolve a 50 µs engine. You must *prove* the
 instrument is quieter than the signal, and *exclude* runs where it wasn't.

- **Correctness that looks like performance.** An engine that violates price-time
 priority can be *faster* (it skips fairness work). If you only measure latency
 and throughput, you reward cheating. Correctness must be a **hard gate**, not a
 weighted nicety.

### 2.2 "Containerize the submission" is a trap if you want latency numbers

The prompt says containerize submissions for isolation. Containers isolate the
*filesystem and namespaces* - but they **share the host scheduler and kernel**.
For a *latency* benchmark that is fatal: a co-scheduled noisy neighbour shows up
as tail latency *in the submission's score*, and you can't tell engine stalls
from scheduler preemption. The correct isolation primitive for a latency
benchmark is a **microVM** (Firecracker): its own kernel, its own scheduling
domain, a clean boundary. (We still use Docker for the *hermetic build* step,
where filesystem isolation is exactly the right tool - §4.)

### 2.3 Cross-protocol comparison is meaningless

Binary (SBE), WebSocket, and REST are not comparable latency populations: TLS handshakes
and JSON framing alone add tens of microseconds before the engine sees a byte.
Ranking a REST engine against a binary-wire engine on raw p99 is a category error. The
platform must rank **per protocol**, never mixed.

### 2.4 Percentiles are not averageable

You cannot average p99s across bots to get a fleet p99 - that is mathematically
meaningless. The only correct fleet-wide percentile comes from **merging the raw
histograms** and reading the percentile off the merged distribution. This forces
an architectural choice: bots must emit *mergeable histograms*, not summary
numbers, and the merge must be **exact**.

**These four realisations - CO, microVM-not-container, per-protocol, no-averaging
- are the design. Everything below follows from them.**

---

## 3. High-Level Design

### 3.1 System at a glance

```
   FROZEN WIRE CONTRACT  (interface_contract_v1.h)
   One binary format, frozen at v1.1. All three
   components compile against it - the seam that
   lets three people build in parallel.
            |
            v
   [T1]  SANDBOX  -  Firecracker microVM
         Isolates the untrusted engine behind a
         real kernel + scheduling boundary, so we
         measure the engine, not host noise.
            |
            v   constant-rate load, SBE binary wire
   [T2]  BOT FLEET + TELEMETRY
         Open-loop, CPU-pinned load generator and
         the gold-standard reference engine.
         Measures CO-corrected tail latency and
         runs the byte-exact correctness diff.
            |
            v   per-second HDR + diffs, scored
   [T3]  LEADERBOARD  -  browser, display-only
         Renders scored deltas over WebSocket.
         Never computes a percentile, so it
         cannot lie.
```

Three components, three owners, **one frozen contract**. The contract is the
parallelization seam: with the wire format frozen on day one, the sandbox team,
the bot/telemetry team, and the leaderboard team each built and tested against a
stable interface and integrated without churn.

### 3.2 Why this decomposition (and the alternatives we rejected)

| Decision | Why | Rejected alternative |
|---|---|---|
| **3 components, 1 frozen binary contract** | Clean ownership; binary wire = zero serialization overhead on the hot path; one header compiles into all three | A shared microservice mesh with gRPC everywhere - adds protobuf encode/decode latency to the exact path we're measuring |
| **Binary SBE-style wire, not JSON/gRPC** | Latency benchmark: the transport must not add measurable latency. Fixed 4-byte header + fixed structs = parse by pointer-cast | JSON (tens of µs framing), gRPC (HTTP/2 + protobuf overhead) - both pollute the measurement |
| **Frozen + versioned contract** | One field change breaks 3 codebases; freezing removes the highest-churn risk in a 3-person parallel build | Living interface negotiated as you go - guarantees integration thrash |
| **Display-only leaderboard** | The UI *cannot* introduce a measurement lie if it is structurally incapable of computing one | A "smart" frontend that recomputes percentiles - reintroduces averaging bugs in the last mile |

### 3.3 End-to-end data flow

1. Contestant submits an engine (source tarball or ELF). The **Sandbox** hashes it
   to an immutable `submission_id`, builds it hermetically, attests it, packs a
   rootfs, and boots it in a Firecracker microVM exposing the SBE wire protocol.
2. **Bot Fleet** drives **constant-arrival-rate** load at the engine, tagging each
   order with its *intended* send time, recording **two** HDR histograms (naive and
   CO-corrected), running an integrity self-test first.
3. Per-second HDR snapshots and correctness diffs flow to the **Telemetry Gateway**,
   which scores and ranks per Interface Contract section 5, reading p99 from the
   **merged additive HDR**, never an average.
4. The gateway diffs each tick and fans out **display-ready deltas** over WebSocket.
5. The **Leaderboard** renders them, and *only* renders them.

### 3.4 Inter-service communication

| Hop | Protocol | Rationale |
|---|---|---|
| Bot ↔ Engine (sandbox) | **Binary SBE over TCP** (custom 4B-framed) | Hot path; zero-copy parse; the thing being measured |
| Bot to Telemetry | **Per-second CSV HDR snapshots** (file) / HDR blobs | Mergeable raw distributions, not summaries; cold path |
| Telemetry to Leaderboard | **WebSocket JSON deltas** | Display layer; human-scale cadence (1-5 s); diff-only payloads |
| Production metrics store | **VictoriaMetrics + additive-HDR blobs** | Time-series at scale; HDR stored mergeable (see §8) |

---

## 4. LLD: Component 1: Submission & Sandboxing Engine (`sandbox/`)

**Goal:** run an untrusted contestant engine such that we can benchmark its
*latency* without trusting it and without polluting the measurement.

### 4.1 The pipeline (`sandbox/pipeline.sh`)

```
INTAKE  ->  BUILD  ->  ATTEST  ->  PACK  ->  ORCHESTRATE (runs the microVM)
```

| Stage | File | What it does | Key decision |
|---|---|---|---|
| **Intake** | `intake/intake.sh` | sha256 to immutable `submission_id`; 50 MB cap; **lint-heuristic** banned-syscall scan; extract | The source scan is explicitly a *lint heuristic, not a security boundary* - it's defeated by `syscall(2)` by number, inline asm, `dlopen`. We documented this honestly in-code. |
| **Build** | `builder/` | Hermetic Docker build, **`--network none`**, static link, `-march=x86-64-v2` | Build isolation is about filesystem and network, so containers are the *right* tool here (unlike runtime). No network means no supply-chain fetch at build time. |
| **Attest** | `attester/attest.sh` | Records a measurement (hash + version) of *what is about to run* | The integrity half of "did we benchmark what they submitted?" |
| **Pack** | `packer/pack_rootfs.sh` | Builds a minimal `ext4` rootfs containing the binary | Minimal attack surface inside the VM |
| **Orchestrate** | `orchestrator/run_vm.sh` | Boots Firecracker: **read-only rootfs**, TAP networking, `panic=1`, `pci=off` | Real kernel + scheduling boundary; read-only root prevents persistence |

### 4.1b Contestant Upload Flow (`platform/intake-api/`)

A contestant interacts with the platform through one HTTP front door — the intake API
(Go), which fronts the pipeline above and tracks each submission through an explicit
state machine. The submission work-queue is backed by **Redis** (`LPUSH submissions:pending`).

```
POST /api/v1/submissions   (multipart tarball, field "engine")
   → state RECEIVED, queued on Redis  submissions:pending
   → 202 { "submission_id": "...", "state": "RECEIVED",
           "status_url": "/api/v1/submissions/<id>" }

GET  /api/v1/submissions/:id    → current state-machine snapshot

State machine (worker advances RECEIVED→BUILDING→ATTESTED itself; the
orchestrator posts RUNNING/SCORED):

   RECEIVED ──▶ BUILDING ──▶ ATTESTED ──▶ RUNNING ──▶ SCORED
                    │            │            │
                    └────────────┴────────────┴──▶ REJECTED   (any stage can reject)
```

Example — upload an engine and poll its status:

```bash
# upload
curl -F engine=@my_engine.tar.gz http://localhost:9090/api/v1/submissions
# → 202 { "submission_id": "a1b2…", "state": "RECEIVED",
#         "status_url": "/api/v1/submissions/a1b2…" }

# poll
curl http://localhost:9090/api/v1/submissions/a1b2…
# → { "submission_id": "a1b2…", "state": "ATTESTED" }
```

This is the missing front door that turns "a folder of pipeline stages" into a
platform a contestant can actually submit to. **Honest status:** the worker advances a
submission as far as **ATTESTED** today; `RUNNING`/`SCORED` are reached once the
orchestrator consumes the scoring queue on a `/dev/kvm` host (Phase 1.4) — the API
contract and state machine are complete, the microVM-execution hand-off is the
hardware-bound step.

### 4.2 The load-bearing decision: Firecracker, not containers

> A benchmark must isolate the **latency** of the submission, not just its
> filesystem. MicroVMs give a clean scheduling and kernel boundary; containers
> share the host scheduler and pollute the tail.

This is the single most important sandbox decision and it directly serves §2.2.
The **enforced security boundary is the runtime seccomp profile + the microVM
guest kernel**, not the intake lint scan. We say so in the code comments so a
reviewer is never misled about where the real boundary is.

### 4.3 Fair resource allocation

CPU pinning and strict memory limits are applied at the *deployment* layer
(K8s Guaranteed QoS + Terraform `isolcpus`, §10) so each submission gets exclusive
cores - the prompt's "fair resource allocation (CPU pinning, strict memory
limits)" requirement is met where it is actually enforceable: the node.

---

## 5. LLD: The Frozen Contract (`contracts/interface_contract_v1.h`)

The seam. Little-endian, x86-64, **FROZEN at v1.1**. A 4-byte `FrameHeader`
followed by one of five fixed-layout structs.

### 5.1 Wire format

```
[ FrameHeader 4B ][ Payload N bytes ]

struct FrameHeader { uint8_t msg_type; uint8_t _pad; uint16_t msg_len; };
```

| msg_type | Message | Payload | On wire | Note |
|---|---|---|---|---|
| 1 | `NewOrder`    | 40B | 44B | bot to engine |
| 2 | `CancelOrder` | 32B | 36B | bot to engine |
| 3 | `OrderAck`    | 32B | 36B | engine to bot |
| 4 | `Fill`        | 56B | 60B | **fits a single 64B cache line** |
| 5 | `Reject`      | 32B | 36B | 7 reason codes |

### 5.2 Three design choices that pay off everywhere

1. **Common header at identical offsets in all 5 types** - `seq @ 0`,
   `timestamp_ns @ 8`, `symbol_id @ 16`. Telemetry extracts these three fields
 from *any* message without knowing its type. This is what makes the diff tool
 and the latency join cheap.
2. **Fixed-point prices (`int64` ticks), never float.** Floating point is
 non-deterministic across compilers/optimisation levels - fatal for a
 byte-exact validator. Ticks make the gold standard reproducible.
3. **`seq` is the latency join key.** Latency for an order =
   `ack.timestamp_ns − NewOrder.timestamp_ns` for the same `seq`. One field, one
 join, no ambiguity.

### 5.3 Contract invariants that the validator depends on

- **Maker Fill emitted before Taker Fill** on every match event. This is
 *contract, not convention* - the byte-exact diff aligns two engines' output
 streams and only works if both order their fills identically.
- **MARKET orders are IOC**: emit Fills for what fills, then **one** Reject
  (`MARKET_INSUFFICIENT_LIQUIDITY`) for residual qty.
- **Engine `seq` is monotonic, global, never reset** - gaps mean the engine
 dropped a message.

---

## 6. LLD: Component 2a: Reference Matching Engine (`order_book.h`)

The gold standard every contestant is diffed against. Header-only so the offline
replay CLI and the in-sandbox TCP server share *identical* logic.

### 6.1 Four invariants (enforced by construction)

1. **Single-writer per symbol** - one thread mutates one book; no locks on the
 match path.
2. **Deterministic** - the same input produces byte-identical output on every run.
   Critical detail: `by_id_` (the `unordered_map` for O(1) cancel lookup) is **never
   iterated**, because hash iteration order is implementation-defined and would
   break determinism. Find, insert, and erase only.
3. **Price-time priority** - sparse `std::map` keyed by price tick — O(active-levels) memory, avoids OOM at distributed scale; byte-exact determinism re-verified in CI;
   each level is a `std::list<RestingOrder>` (FIFO). Match walks the front of the
   best level.
4. **Maker-before-taker fill emission** - per §5.3, a contract invariant.

### 6.2 Why this data structure

| Choice | Why | Trade-off accepted |
|---|---|---|
| Sparse `map` over price range `[1, 1e6]` | O(log N) price to level; O(active-levels) memory | Avoids OOM at distributed scale; byte-exact determinism re-verified in CI |
| `std::list` per level | O(1) FIFO insert at back, O(1) erase via stored iterator | Pointer-chasing per match - acceptable; the reference engine is the *correctness* oracle, not the speed target |
| `unordered_map<seq, {price_idx, list_iterator}>` | O(1) cancel | Must never be iterated (determinism) - enforced and documented |
| Collapse `engine_seq` and `engine_timestamp` into one counter | Cleaner schema; both advance identically per emit | Slightly unusual, documented as deliberate |

### 6.3 The byte-exact validator (`refengine diff`)

The strongest correctness claim in the system. The diff walks two output
journals frame-by-frame and reports the **first divergent byte** with a decoded
message on both sides. No heuristics, no thresholds - either the bytes match or
they don't.

The demo plants a **real-world HFT bug**: LIFO matching (newest order fills first)
instead of FIFO - a price-time-priority violation that exchanges have shipped to
production. It produces a journal the *same length* as the correct one, so a
threshold checker would pass it. Our diff pinpoints it:

```
DIVERGE @ byte 244: A=0x01 B=0x05
  A: message #7 (Fill, engine_seq=7, order_seq=1)   [oldest order matched - correct]
  B: message #7 (Fill, engine_seq=7, order_seq=5)   [newest order matched - the bug]
```

---

## 7. LLD: Component 2b: Bot Fleet + Telemetry (`bot.cpp`): the centerpiece

An **open-loop, multi-threaded, CPU-pinned** load generator. Every design choice
serves one of two goals: (1) measure latency honestly under stalls, and (2) be
cheap enough on the hot path that the instrument is quieter than what it measures.

### 7.1 Coordinated Omission correction (the heart of the platform)

The structural Tene/Snyder fix:

- The hot loop **pre-computes the next intended send time** and catches up if it
 falls behind. Each in-flight order is tagged with its **intended** send time,
  not wall-clock at `send()`.
- On ack, latency = `now − intended`. A stall therefore shows up as *latency*,
 not as *missing samples*.
- A **second histogram** receives the same value via
  `hdr_record_corrected_value(latency, expected_interval)`, which backfills the
 phantom samples a stall would otherwise omit.
- `SO_SNDBUF` is pinned **small on purpose** so backpressure is visible, not
 hidden in kernel buffers.

Both histograms are reported **side by side**, so the CO gap is *provable*, not
asserted. (See §11.4 for the verified 19.7× gap.)

### 7.2 Honest measurement under load

| Technique | What & why |
|---|---|
| **CPU pinning** (`pthread_setaffinity_np`, skip core 0) | Without it the scheduler ping-pongs workers across cores, causing roughly 1 ms of preemption jitter that pollutes p99. With it, only *real* stalls are visible. macOS lacks this API, so the bot detects it and degrades gracefully (documented). |
| **`HFT_PAUSE` / `_mm_pause()`** in the catch-up spin | ~30-cycle hint; deprioritises the SMT sibling and avoids the memory-order-violation flush when the spin exits. `yield` on ARM, no-op elsewhere. |
| **`SO_BUSY_POLL`** (Linux) | Kernel spins on the NIC RX queue instead of waiting for IRQ wakeup - documented 5-20 µs win. `EPERM`-safe without `CAP_NET_ADMIN`. |
| **`SO_TIMESTAMPING`** (Linux) | Requests HW TX/RX timestamps for 4-way latency decomposition (bot SW \| net out \| engine \| net in). Falls back to SW timestamps; full cmsg parse is a production step needing a PTP-synced NIC. |
| **Per-thread state, zero shared writes on hot path** | Each worker owns its socket, `OrderPool` (1024 preallocated slots), `xorshift64` RNG, 1M-slot pending map, and HDR histograms. The hot path allocates **nothing** after startup. |
| **Additive HDR merge at the end** | HDR histograms are *provably additive* - merging per-thread histograms with `hdr_add()` is **exact, not approximate**. This is why fleet percentiles are real (§2.4). |

### 7.3 Integrity gate (the instrument polices itself)

Before the scoring run, the bot runs a **1,000-order self-test** against the
target. If the bot's *own* p99 exceeds `--gate-p99-us` (default 1,000 µs), the
run is flagged `INTEGRITY: FAILED` and exits non-zero - so the leaderboard can
**auto-filter** it. The measuring instrument must beat what it measures, and we
*prove* it run-by-run. On the leaderboard this surfaces as **exclusion, not
penalty** (§8.2).

### 7.4 SPSC lock-free HDR offload (opt-in, `--use-spsc`)

A Vyukov-style single-producer/single-consumer ring (cache-line-aligned
head/tail; relaxed/acquire/release ordering). The hot path does **one relaxed
atomic store**; a cold consumer thread feeds the histograms. At our scale
(~10 k/s/bot) the saving is microscopic and we say so - this exists to **prove the
architecture scales to 1 M+ msg/s**, where the offload becomes critical. CI
asserts `dropped == 0` so the offload path can never silently diverge from the
inline path.

### 7.5 Robustness details (the unglamorous correctness)

- `recv() == 0` (clean peer close) detected to no 100%-CPU infinite spin if an
 engine dies.
- Initial `connect()` bounded by a 3 s timeout so a blackholed IP can't hang a
 worker in-kernel forever; timeouts cleared after connect so the non-blocking
 hot path is untouched.
- `--warmup-orders N` excludes TCP slow-start / ARP / page-fault / cache-warm
  spikes from the histograms (throughput counters unaffected) - standard `wrk2`
 practice.

---

## 8. LLD: Component 3: Real-Time Leaderboard + Telemetry Gateway

### 8.1 Cardinal rule: display only

The leaderboard **never recomputes a percentile** - no averaging, no
re-bucketing, no interpolation. It renders the contract's already-CO-corrected,
additively-merged gauges. This is a *structural* guarantee against the last-mile
measurement lie: the UI is incapable of introducing one.

### 8.2 Telemetry Gateway (`tools/telemetry_server.js`)

The reference gateway (Node.js) is the scoring + ranking + fan-out service. The
production gateway (Rust/Go reading **VictoriaMetrics** + additive-HDR blobs) is a
**drop-in replacement that emits the same JSON** - the frontend never changes.

- **Additive HDR** mirrored server-side (80 log-spaced buckets, 500 ns-500 ms) so
 scoring reads p99 from the **merged** histogram, never a mean. Within a single bot, histograms are merged with hdr_add() - exact and additive. Across bots and over time, hdr_merge ranks by max-of-percentiles (conservative); we never average percentiles.
- **Scoring (pure function, Interface Contract §5):**
  `score = 0.40*latency + 0.30*throughput + 0.30*correctness`, min-max normalised
 per protocol board, with a **hard-fail cap**: a correctness failure
  (`diff_pass_rate < 0.999` or `invariant_violations > 0`) caps the score -
 **speed never buys back incorrectness** (§2.1).
- **Integrity gate to exclusion before ranking.** Submissions whose self-test p99
 or software jitter failed are *excluded*, not penalised - a filter, surfaced in
 the UI as a greyed row.
- **Per-protocol boards** (BINARY (SBE v1) / WS / REST), never mixed (§2.3). An *Overall* view
 exists but is gated behind a warning.
- **Delta streaming**: the gateway diffs each tick (cadence 1-5 s) and the UI
 animates only what changed. Immediate snapshot on connect so the board is never
 blank.

### 8.3 Mock-first, zero-change live swap

The UI runs against a **synthetic, contract-faithful** source by default; setting
`USE_LIVE=true` subscribes to the WebSocket feed. If the socket fails it silently
falls back to mock - **a demo is never blank**. The synthetic source uses the same
roster/shape as the live feed, so mock and live are visually identical.

> *Reliability engineering note:* the public showcase page (`landing.html`) hardens
> this further - it self-hosts all JS libraries (no CDN dependency, so ad-blockers
> can't strip the visualization), keeps a CDN-free CSS fallback for the WebGL core,
> and a 3-second boot watchdog guarantees content is visible no matter what loads.

### 8.4 Full percentile ladder

p50 to p90 to p99 to p99.9 to p99.99 to max, **in nanoseconds**. No mean-latency
headline anywhere - an average hides exactly the stalls that matter.

---

## 9. Architecture Decision Records (ADRs): alternatives & trade-offs (rubric #2 & #3)

Each row below is an **Architecture Decision Record (ADR)** — a real fork in the road,
the path taken, the path rejected, and the reason. They are numbered ADR-1 … ADR-11 so
they can be cited individually.

| ADR | Decision | Chosen | Alternative(s) rejected | Why |
|---|---|---|---|---|
| ADR-1 | Measurement methodology | **CO-corrected (Tene/Snyder)** | Naive round-trip histograms | Naive numbers understate the tail by 19.7× (tabulated shared-host run) up to 76× (measured isolcpus); they'd rank engines incorrectly |
| ADR-2 | Submission isolation (runtime) | **Firecracker microVM** | Docker container | Container shares the host scheduler, polluting the *latency* being measured |
| ADR-3 | Build isolation | **Docker `--network none`** | Build on host | Hermetic, no supply-chain fetch; filesystem isolation is the right tool *here* |
| ADR-4 | Wire format | **Binary SBE, 4B-framed** | JSON / gRPC | JSON/gRPC add tens of µs to the path we're measuring |
| ADR-5 | Contract lifecycle | **Frozen + versioned v1.1** | Living interface | One field = 3 codebases broken; freezing removes the top integration risk |
| ADR-6 | Fleet percentile | **Merge raw HDR, read p99** | Average per-bot p99s | Averaging percentiles is mathematically meaningless |
| ADR-7 | Correctness | **Byte-exact diff, hard-fail cap** | Threshold / sampling checker | A same-length LIFO bug passes a threshold checker; byte-exact catches it |
| ADR-8 | Latency clock | **Single-clock RTT (`now − intended_send` on the bot) + CO correction** | Cross-machine PTP / HW timestamp diff | One clock means nothing to sync or drift; the honest tail comes from CO back-fill, not clock precision. PTP is a production-roadmap step, not required for a correct RTT. |
| ADR-9 | Leaderboard role | **Display-only** | Frontend recomputes metrics | Removes the last-mile lie by construction |
| ADR-10 | Integrity gate result | **Exclude, not penalise** | Score penalty | A penalty still shows a number we can't stand behind |
| ADR-11 | Price representation | **`int64` ticks** | `double` | Float is non-deterministic and breaks byte-exact replay |

### 9.1 Deliberate deferrals (honest scope: *decisions, not gaps*)

| Deferred | Why it doesn't change the score *at this scale* |
|---|---|
| **AVX-512 / SIMD batch ingester** | Payoff starts at ~1 M msg/s receive; current per-bot rate ~10 k/s. The SPSC path proves the architecture scales there. |
| **AF_XDP kernel bypass** | Needed for sub-µs receive; measured bare-metal p99 is already 7.7 µs on TCP + `SO_BUSY_POLL` (isolcpus). |
| **eBPF in-kernel latency prober** | A kernel-side probe measures a *different* number (kernel ingress→egress), not the trader-visible RTT. Our single-clock userspace RTT + CO correction is the honest, trader-relevant measurement; see §11.8. A deliberate choice, not a missing feature. |
| **Aeron / Chronicle transport** | Our topology is N-bots to 1-engine fan-in, not 1-publisher to many-subscribers. Wrong tool. |
| **HW NIC timestamp cmsg parse** | Request path is wired; full parse needs a PTP-synced production NIC. |
| **macOS CPU pinning** | `pthread_setaffinity_np` is Linux-only; the bot detects and degrades gracefully. |

Documenting these as engineering judgment (with the scale threshold at which each
would matter) is the difference between "we ran out of time" and "we knew exactly
where the line was."

---

## 10. Deployment & Infrastructure as Code (`infra/`)

![AWS deployment architecture](docs/architecture_diagram.png)

Two paths, by purpose - and we're honest about which produces real numbers.

### 10.1 Local / demo: `docker-compose.yml`

```bash
docker compose -f infra/docker-compose.yml up --build
open http://localhost:8088 # leaderboard
# live feed: ws://localhost:8080/leaderboard/deltas
```

Four services: `reference-engine`, `bot-fleet`, `telemetry-gateway`,
`leaderboard`. **Correctness & demo only** - containers add scheduler/netns jitter
that pollutes the tail; the bot's Linux perf features no-op here and degrade
gracefully. We state this explicitly rather than presenting compose numbers as
real.

### 10.2 Production: `k8s/platform.yaml` + `terraform/benchmark_pool.tf`

The low-latency path, where the Linux-only features actually engage:

- **Bot fleet = bare-metal `DaemonSet`**, `hostNetwork: true` (skip CNI overlay on
  the hot path), `capabilities: [NET_ADMIN]` (for `SO_BUSY_POLL`/`SO_TIMESTAMPING`),
  **Guaranteed QoS** (`requests == limits`, integer CPU to exclusive cores via the
 kubelet static CPU-manager policy).
- **Benchmark nodes (Terraform)**: provisioned via `dmacvicar/libvirt` - `terraform apply` boots real KVM VMs (`verified_runs/aftab/terraform_apply.txt`), kernel
  cmdline `isolcpus=1-15 nohz_full=1-15 rcu_nocbs=1-15`, plus a **label + taint**
 so only benchmark workloads land there.
- **Telemetry gateway** Deployment + Service (`/health` readiness); **leaderboard**
 Deployment (2 replicas) + LoadBalancer.

**Honest status:** compose structure/ports/healthchecks are verified by
inspection and an end-to-end WS client test; the full stack was not run in CI
(no Docker-in-sandbox). Terraform is **applied** (3 libvirt VMs booted, `verified_runs/aftab/terraform_apply.txt`); GHCR images are **published** (`ghcr.io/mohitt31/iicpc-*`, built green in CI). The K8s manifest proves horizontal scale-out shape, not yet applied to a live
cluster here. We label blueprints as blueprints.

---

## 11. Verified Results / Performance Characteristics (rubric #6: every number is reproducible)

_This section is the platform's **Performance Characteristics**: every figure is sourced
to a committed run in `verified_runs/` and to `verified_runs/canonical.json`._

**Performance Characteristics at a glance** (all from `canonical.json`):

| Characteristic | Measured value | Source |
|---|---|---|
| Coordinated-Omission gap (p99) | 19.7× tabulated (shared host) → 76× (isolcpus) | `verified_runs/aftab/co_proof.txt` |
| Bare-metal latency p99 | **7.7 µs MEASURED** (i7-13620H, isolcpus) | `verified_runs/aftab/baremetal_latency.txt` |
| Byte-exact determinism | live == offline replay, **IDENTICAL, exit 0** (representative 25,743,624 bytes; run-dependent) | `verified_runs/aftab/live_replay.txt` |
| Fleet scale | 2,989 connections, Sent == Acked, 0 aborts | `verified_runs/aftab/scale_3000bots.txt` |
| Correctness | 19/19 unit tests; planted LIFO bug caught at byte 244 | `RESULTS.md §2` |
| Memory safety | 0 ASan / 0 UBSan findings | `RESULTS.md §11.6` |

Environment for the headline shared-host runs: a **4-core shared cloud container**
(Linux x86-64, **no core isolation, no NIC tuning**) - deliberately hostile. See
§11.7 on why that's a feature.

### 11.1 Correctness: 19/19 unit tests
`cd bot-engine/build && ./test_order_book` to `=== Results: 19/19 passed ===`
(price-time priority, partial fills, IOC market orders, cancels, duplicate-seq
replay protection, maker-before-taker order, engine-seq monotonicity, full-run
determinism).

### 11.2 Correctness: byte-exact validator catches the planted bug
`./demo/run_demo.sh` to `DIVERGE @ byte 244` pinpointing the LIFO price-time
violation on message #7 (the first aggressive fill).

### 11.3 Correctness: live TCP == offline replay, byte-for-byte
200,000 orders fired over TCP at the reference server; its journal diffed against
an offline replay of the captured input:
` to  IDENTICAL: 25,743,624 bytes match`. (200k sent = 200k acked, 369,785 CO samples,
0 partial sends / collisions / pool exhaustion.) The gold standard cannot
disagree with itself - *that* is what makes contestant scoring trustworthy.

### 11.4 The centerpiece: Coordinated Omission proof (600k orders)
Deterministic 5 ms stall injected every 20,000 orders:

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

naive p99 ~173 µs vs CO-corrected p99 ~3.41 ms - a 19.7x gap. The corrected tail reproduces our earlier 3.43 ms measurement within 0.5%; the ratio is host-dependent.

### 11.5 SPSC offload: same truth off the hot path
`SPSC: dropped 0 records` over 300,000 (CI asserts `dropped 0` every push).

### 11.6 Memory safety + concurrency abuse
- **ASan + UBSan**: full suite rebuilt with sanitizers, re-run end-to-end to 
 **0 findings** (the hot path allocates nothing after startup, so there's nothing
 to leak - now proven, not claimed).
- **32 bots on 4 cores** (8x oversubscription): `1,279,594 / 1,279,594 sent=acked, 0 partial aborts, 0 collisions` (16-core host; threads 16-32 ran unpinned, handled gracefully). Backpressure surfaces as `EAGAIN`, never as lost or double-counted messages.
- **Soak**: Across all committed runs (600k CO proof + 200k replay + 1.28M in the 32-bot test = 2M+ orders), zero integrity-counter violations (0 collisions, 0 pool exhaustion, 0 partial aborts). The full soak suite (soak/soak_test.sh) is reproducible.

### 11.7 Reading the numbers honestly
These ran in a shared container with rampant scheduler preemption. On a quiet, otherwise-idle machine, naive and CO-corrected p99 agree within ~1.1x. 
In our shared CI container, even 'clean' runs show inflated CO tails - that is the 
methodology correctly reporting real scheduler stalls, not a bug. *A benchmark that shows beautiful numbers
in a noisy environment is broken. This one refuses to.*

### 11.8 Latency Measurement: single-clock RTT + CO, not an eBPF kernel prober

**What we measure and where.** Latency is the trader-visible round trip: the bot
timestamps `intended_send` and `ack_received` on its **own single clock** and reports
the difference, then applies Coordinated-Omission correction so a stalled engine cannot
hide missed sends. One clock means there is nothing to synchronize, drift, or
mis-configure — the honesty of the tail comes from the CO back-fill, not from clock
precision (ADR-8).

**Why not an in-kernel eBPF prober.** A competing approach instruments the kernel
(eBPF/kprobes) to time the syscall or NIC path. We deliberately did **not** do this, and
it is a decision, not a gap:

- An eBPF probe measures a *different quantity* — kernel ingress→egress inside the host —
  not the round trip a trading client actually experiences. Optimizing the number a judge
  cares about means measuring the trader-visible RTT, which is exactly what single-clock
  RTT captures.
- The CO correction is the hard, rare part of honest latency measurement, and it lives in
  userspace regardless of where the timestamp is taken. eBPF would add kernel-version
  fragility and root/privilege requirements without changing the reported truth.
- Kernel-bypass / in-kernel timing (eBPF, AF_XDP) is recorded as a **documented deferral**
  (§9.1): it matters below ~1 µs receive, where our measured 7.7 µs bare-metal p99 is not
  yet the binding constraint.

This is the same honesty discipline as the rest of the platform: report the number the
user feels, by the simplest mechanism that cannot lie about it.

### 11.9 Chaos Engineering / Resilience

The platform is exercised under deliberate failure, not just happy-path load:

- **Engine-kill mid-run (DONE, committed):** the matching engine is killed during a live
  benchmark; backpressure surfaces as `EAGAIN`, Sent == Acked accounting holds, zero lost
  or double-counted orders. Log: `verified_runs/aftab/resilience_engine_kill.txt`.
- **Node-kill / DaemonSet reschedule (PLANNED):** kill a benchmark node mid-run; the fleet
  reschedules and the board shows the gap honestly. Pending the multi-node k3s run (Phase 2.3).
- **Gateway reconnect (BUILT, demo-pending):** the leaderboard's silent fallback + reconnect
  path already exists in `tools/telemetry_server.js`; a recorded clip is pending.
- **Integrity-gate auto-filter (DONE, mechanism):** a run that fails self-test latency or
  jitter bounds is excluded before scoring (ADR-10), demonstrated by the gate logic; a
  live on-camera clip is pending.

Status is stated per item — committed runs are cited; planned/pending clips are labeled as
such, never implied as done.

---

## 12. Technology Choices (and why)

| Layer | Tech | Why |
|---|---|---|
| Bot / engine / validator | **C++20** | Deterministic control over allocation, affinity, intrinsics on the hot path |
| Latency timing | **`rdtscp` invariant-TSC** to ns | Sub-ns resolution, no syscall on the hot path |
| Histograms | **HDR (additive)** | Mergeable & exact; the only mathematically sound fleet percentile |
| Sandbox runtime | **Firecracker microVM** | Real scheduling/kernel boundary for latency isolation |
| Build isolation | **Docker `--network none`** | Hermetic builds |
| Telemetry transport | **WebSocket JSON deltas** | Human-cadence display layer; diff-only payloads |
| Metrics store (prod) | **VictoriaMetrics** | High-cardinality TS at scale; HDR stored mergeable |
| Orchestration | **Kubernetes + Terraform** | Guaranteed-QoS pinned cores; reproducible isolated node pool |
| Leaderboard | **Static HTML/JS (Three.js + GSAP)** | Zero-backend display layer; self-hosted libs for reliability |

---

## 13. Production Roadmap

1. **Telemetry gateway in Rust/Go** reading VictoriaMetrics + additive-HDR blobs
 (same JSON contract to frontend unchanged).
2. **HW NIC timestamp cmsg parsing** on a PTP-synced fleet for true 4-way latency
 decomposition.
3. **AF_XDP / SIMD ingest** once per-node receive crosses ~1 M msg/s (SPSC path
 already proves the seam).
4. **Multi-symbol sharded books** with one writer thread per symbol shard.
5. **Submission queue + scheduler** for fair multi-tenant benchmark slotting.

---

## 14. Appendix: Reproduce everything

```bash
# Build
cd bot-engine && mkdir -p build && cd build && cmake .. && make -j && cd ..

# Correctness: unit tests, planted-bug diff, live==replay
./build/test_order_book
./demo/run_demo.sh

# The centerpiece: Coordinated Omission proof
./build/null_responder --stall-mode --stall-every 20000 --stall-ms 5 &
./build/bot --bots 1 --interval-us 100 --duration-sec 60 --no-gate ; kill %1

# Production-confidence soak (~9 min) and integration smoke (~12 s)
./soak/soak_test.sh
./test/integration_test.sh

# Live leaderboard feed
cd ../tools && npm install && node telemetry_server.js --port 8080 &
cd ../frontend && python3 -m http.server 8088 # to http://localhost:8088
```

Full per-component design notes: `ARCHITECTURE.md`, `bot-engine/README.md`,
`contracts/INTERFACE_CONTRACT.md`. Verified-results log: `RESULTS.md`.

---

*One sentence to leave you with: anyone can draw a leaderboard. We built the one
that refuses to show you a number it can't stand behind.*

---

## 15. Hostile-Judge Q&A (defense annex)

The questions a systems-literate judge is most likely to push on, and the honest
answers. Every claim here is sourced to a committed run (`verified_runs/`,
`canonical.json`) or to the section noted.

### Q1. "No kernel bypass (AF_XDP / DPDK / eBPF)? Real HFT shops use it — isn't that a gap?"

It's a **deliberate deferral**, recorded in §9.1, not an oversight — and it does not
change the ranking at this scale:

- The platform's job is to rank *contestant engines* by the latency a trading client
  actually experiences. That ranking is determined by the engine's own matching path,
  not by our transport's last microsecond. Kernel bypass would lower every engine's
  measured floor by the same offset; the **ordering is unchanged**.
- Our measured bare-metal p99 is **7.7 µs** on TCP + `SO_BUSY_POLL` (isolcpus,
  i7-13620H — `verified_runs/aftab/baremetal_latency.txt`). AF_XDP earns its complexity
  below ~1 µs receive, which is not the binding constraint here.
- The hard, rare part of honest latency measurement is **Coordinated-Omission
  correction**, which lives in userspace regardless of transport. We spent the
  engineering budget there, where it changes the truth, not on a transport optimization
  that changes every row equally. (See also §11.8 on why an in-kernel eBPF prober
  measures a *different quantity* than the trader-visible RTT.)

### Q2. "How do you stop a contestant from gaming the benchmark?"

Three independent layers, each defeating a different cheat:

1. **Attestation (`attester/attest.sh`)** records a hash + version of *exactly what is
   about to run*, so a contestant cannot submit one binary and have a different one
   benchmarked. It answers "did we measure what they actually gave us?"
2. **Integrity gate** — the instrument polices itself: a run whose self-test latency or
   jitter exceeds bounds is **excluded before scoring, not penalised** (ADR-10). You
   can't farm a lucky low number out of an unstable run.
3. **Byte-exact correctness diff** — the engine's output journal is compared
   byte-for-byte against an offline replay through the gold reference engine. A subtly
   wrong engine that produces *plausible* numbers is caught: our planted LIFO
   (price-time-priority) bug diverged at **byte 244** and was flagged, even though both
   journals were the same length (§6.3). A threshold/sampling checker would have passed it.

A contestant would have to defeat all three simultaneously — submit-bait-and-switch,
run stably, *and* be byte-exact-correct — at which point they haven't gamed it, they've
just built a correct fast engine, which is the thing we're trying to find.

### Q3. "Why Firecracker microVMs instead of containers? Containers are simpler."

Because we're measuring **latency**, and the isolation primitive has to not pollute the
thing being measured (ADR-2, §4.2):

- A container isolates filesystem and namespaces but **shares the host kernel and
  scheduler**. A noisy neighbour's scheduling decisions leak directly into the
  contestant's tail latency — the exact number we report. The measurement would be a
  property of *our* host load, not their engine.
- A Firecracker microVM brings its **own kernel and its own scheduling boundary**, so
  the contestant's p99 reflects their engine, not our colocation. Read-only rootfs +
  `panic=1` + `pci=off` keep the attack surface minimal.
- Containers remain the *right* tool for **build** isolation (`--network none`, hermetic,
  no supply-chain fetch — ADR-3), which is about filesystem/network, not scheduling.
  Different problem, different tool. The evidence that scheduler noise is real: our
  shared-container runs show inflated CO tails that vanish on isolated cores (§11.7) —
  precisely the contamination a container would bake into a contestant's score.

### Q4. "Clock sync — how do you trust cross-machine timestamps without PTP?"

We don't need cross-machine sync, because we don't take cross-machine timestamps
(ADR-8, §11.8):

- Latency is measured as **single-clock RTT**: the bot stamps `intended_send` and
  `ack_received` on its **own** clock and reports the difference. One clock means there
  is nothing to synchronize, drift, or mis-configure — a whole class of measurement bugs
  is removed by construction.
- The honesty of the **tail** does not come from clock precision; it comes from
  **Coordinated-Omission correction** back-filling the sends a stalled engine prevented.
  That's the part that's usually wrong in naive benchmarks, and it's independent of how
  the clock is read.
- PTP / hardware NIC timestamping is a **production-roadmap** item (§9.1), useful for
  decomposing where time goes in a deployed system — not required for a correct,
  trader-relevant RTT in this benchmark.

### Q5. "Your WebSocket, FIX, and REST boards — are those real measurements?"

Stated plainly so we're never caught overclaiming, in two parts:

- **The loops are real and integrity-verified.** WebSocket (RFC-6455), FIX 4.4, and REST
  bots each round-trip the same SBE order through the **actual reference engine** via a
  transport adapter, and `wsrest-build.yml` asserts **Sent==Acked** in CI for all three.
  That part is committed and reproducible — the boards are not synthetic mock-ups.
- **The absolute latencies are representative, not yet measured.** The CI smoke proves
  integrity (Sent==Acked) but does **not** capture a committed p99, and these run on a
  shared CI runner, not an isolated host. So the WS 223 µs / FIX 890 µs / REST 2,404 µs
  figures are **representative of the framing-overhead ordering** (binary ≪ WS ≪ FIX ≪
  REST), not a committed isolated-host measurement like the 7.7 µs binary number. The
  **ordering** is the robust result; a committed per-protocol latency run logged to
  `verified_runs/` is the pending step that promotes them from representative to measured —
  the same evidence bar as every other number here.

We would rather state "representative, ordering-robust" than dress a shared-runner number
as an isolated-host measurement. That discipline is the entire point of the platform.

