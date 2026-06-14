# IICPC Summer Hackathon 2026 — Design Document (HLD + LLD)
## A Distributed Benchmarking & Hosting Platform for Trading Infrastructure

> **Thesis in one line.** The hard part of a latency benchmark is not drawing the chart — it
> is **not lying in the chart**. The whole platform is built around one idea: **measure the
> truth, then only display it.**

> **Reading guide.** This is both High-Level (HLD) and Low-Level Design (LLD). Every headline
> number is reproducible and traces to a committed log in `verified_runs/` and to the
> single-source-of-truth `verified_runs/canonical.json`. We optimised **depth over breadth**:
> three *correct* components over four half-built ones, with the one bug that invalidates
> almost every public latency benchmark — **Coordinated Omission** — as the spine.

## Table of Contents
1. System Overview
2. High-Level Architecture
3. End-to-End Data Flow
4. Sandbox Engine
5. Latency Measurement — single-clock RTT + CO correction (why not eBPF)
6. Bot Fleet
7. Telemetry & Validation
8. Real-Time Leaderboard
9. Chaos Engineering & Resilience
10. Inter-Service Communication
11. Data Stores
12. Infrastructure as Code
13. CI/CD Pipeline
14. Composite Scoring Algorithm
15. Technology Decisions
16. Architecture Decision Records & Deliberate Deferrals
17. Performance Characteristics
18. Contestant Upload Flow
19. Final Delivery Summary
   - Appendix A — Hostile-Judge Q&A
   - Appendix B — Reproduce everything

> **Rubric map:** 1 Engineering excellence → §1, §2, §5, §6 · 2 Product decisions → §2, §4,
> §8, §14 · 3 Approaches & trade-offs → §16 · 4 End-to-end → §3, §10, §11, §12 · 5
> Deploy-ready → §12, §13, §18 · 6 Tests verified → §17, §9.

---

## 1. System Overview

We were asked to build a platform where contestants upload a matching engine, it is hosted
under isolation, a distributed bot fleet bombards it, and it is ranked live on latency,
throughput, and correctness. We built exactly that, as **three independently-developed
components bound by one frozen wire contract**:

1. **Sandbox** (`sandbox/`) — a Firecracker-microVM pipeline that runs an untrusted submission
   behind a real scheduling/kernel boundary **with an enforced seccomp syscall filter**
   (`sandbox/packer/init/seccomp_init.c`), so we benchmark the submission's *latency* and a
   malicious submission is *killed*, not merely contained.
2. **Bot Fleet + Telemetry** (`bot-engine/`) — an open-loop, CPU-pinned C++ load generator
   that measures tail latency **honestly** (Coordinated-Omission corrected), plus a
   gold-standard reference engine and a **byte-exact** correctness validator.
3. **Real-Time Leaderboard** (`frontend/`) — a display-only board that streams scored deltas
   and **never recomputes a percentile** — it cannot lie because it only renders what scoring
   already proved.

The seam is `contracts/interface_contract_v1.h` — a **frozen, versioned (v1.1, `STATUS:
FROZEN`)** binary wire format. One field change breaks three codebases, so we froze it on day
one; that freeze let three people build in parallel without integration hell.

**The problem, as we actually read it.** A benchmarking platform produces *rankings that decide
a winner*. If the numbers are wrong it is worse than useless — it confidently rewards the wrong
engine. So the dominant requirement is **measurement integrity**, not UI polish. Four
realisations drive the design:

- **Coordinated Omission (CO).** A naive bot sends, waits for the ack, records, sends again. If
  the engine stalls 5 ms the bot *also* stalls — the samples that should have landed during the
  stall are never taken; the one sample after reads near-normal. The histogram says **p99 = 30
  µs** while reality was **5 ms**. CO-blind ranking ranks engines wrong. **This is the
  centerpiece.**
- **microVM, not container.** Containers share the host scheduler/kernel; a noisy neighbour
  shows up as the contestant's tail latency. For a *latency* benchmark that is fatal. The right
  primitive is a **microVM** — own kernel, own scheduling domain.
- **Per-protocol, never mixed.** Binary (SBE), WebSocket, FIX, REST are not comparable latency
  populations — framing alone adds tens of µs before the engine sees a byte. Ranking REST vs
  binary on raw p99 is a category error.
- **Percentiles are not averageable.** The only correct fleet p99 comes from **merging raw HDR
  histograms** and reading the percentile off the merged distribution.

These four are the design. Everything below follows.

---

## 2. High-Level Architecture

### 2.1 Logical decomposition
```
   FROZEN WIRE CONTRACT (interface_contract_v1.h, v1.1) — all three compile against it
            │ SBE binary wire
   [T1] SANDBOX        Firecracker microVM + enforced seccomp: real kernel + sched boundary
            │
   [T2] BOT FLEET + TELEMETRY   open-loop, CPU-pinned C++; CO-corrected tail; gold reference
            │ HDR CSV + diffs    engine; byte-exact diff
            │
   [T3] LEADERBOARD   display-only; renders scored deltas over WebSocket; never computes a
                      percentile → cannot lie
```

### 2.2 Physical / deployment topology (AWS)
```
AWS us-east-1
 ├─ Internet Gateway ─▶ Application Load Balancer (Public Subnet "DMZ")
 │      HTTPS/WSS in · upload ELF / WS deltas out · NAT Gateway egress-only
 ├─ Private Zone (no public ingress)
 │   ├─ Services Subnet (EKS): Intake&Build (Go+Docker --network none) ·
 │   │     Telemetry Gateway (score+fan-out) · Leaderboard UI · VictoriaMetrics
 │   └─ Benchmark Subnet (bare-metal isolated): EC2 c6i.metal,
 │         isolcpus+nohz_full+rcu_nocbs · Bot Fleet DaemonSet (pinned cores)
 │         ⇄ Firecracker microVM (contestant engine, RO rootfs + seccomp)
 │         · Reference Engine + byte-exact Diff
 └─ S3 Artifact Store (submission artifact + attestation)
```
Two zones encode a real decision: **everything that touches a contestant's latency lives on
isolated bare-metal**; everything else (intake, scoring, display) lives on ordinary EKS where
jitter can't pollute a measurement.

### 2.3 Why this decomposition (alternatives rejected)
| Decision | Why | Rejected |
|---|---|---|
| 3 components, 1 frozen binary contract | Clean ownership; binary wire = zero serialization on the hot path | gRPC mesh — adds protobuf encode/decode to the measured path |
| Binary SBE wire | Transport must not add measurable latency; 4B header + fixed structs = parse-by-pointer-cast | JSON / gRPC — tens of µs framing |
| Frozen + versioned contract | One field = 3 codebases broken; removes top integration risk | Living interface — guarantees thrash |
| Display-only leaderboard | UI structurally incapable of computing a lie | Smart frontend recomputing percentiles — last-mile averaging bugs |
| Isolated bare-metal benchmark subnet | Latency isolation only real on scheduler-removed cores | Shared EKS nodes — bakes neighbour jitter into the score |

---

## 3. End-to-End Data Flow
1. **Submit.** Contestant POSTs an engine; Sandbox hashes it to an immutable `submission_id`,
   builds hermetically (`--network none`), attests (hash+version of *what runs*), packs a
   minimal ext4 rootfs, boots it in Firecracker. Artifact → S3.
2. **Drive load.** Bot fleet drives **constant-arrival-rate** load, tagging each order with its
   *intended* send time, recording **two** HDR histograms (naive + CO-corrected). Runs a
   1,000-order integrity self-test first; aborts if its own jitter is too high to trust.
3. **Score.** Per-second HDR CSVs + correctness diffs → Telemetry Gateway, which scores/ranks
   reading p99 from the **merged additive HDR**, never an average.
4. **Fan out.** Gateway diffs each tick (1–5 s) → display-ready deltas over WebSocket.
5. **Render.** Leaderboard renders them — and *only* renders them.

**Two data planes, deliberately separated.** Hot plane (bot↔engine) = binary SBE/TCP,
zero-copy. Cold plane (telemetry→gateway→UI) = mergeable HDR + WS JSON at human cadence. The
measurement never rides the display plane.

---

## 4. Sandbox Engine (`sandbox/`)
**Goal:** run an untrusted engine so we can benchmark its *latency* without trusting it and
without polluting the measurement.

### 4.1 Pipeline (`sandbox/pipeline.sh`): `INTAKE → BUILD → ATTEST → PACK → ORCHESTRATE`
| Stage | What | Key decision |
|---|---|---|
| Intake (`intake/intake.sh`) | sha256→`submission_id`; 50 MB cap; lint-heuristic syscall scan; extract | The scan is *explicitly a lint heuristic, not a security boundary* — defeated by `syscall(2)` by number, inline asm, `dlopen`. Documented in-code; the real boundary is runtime (§4.3). |
| Build (`builder/`) | Hermetic Docker, **`--network none`**, static link, `-march=x86-64-v2` | Filesystem+network isolation → container is the right tool here. No network = no supply-chain fetch. |
| Attest (`attester/attest.sh`) | Records hash+version of *exactly what runs* | The integrity half of "did we benchmark what they submitted?" |
| Pack (`packer/pack_rootfs.sh`) | Minimal ext4 rootfs; compiles the seccomp PID-1 to `/init` | Minimal attack surface |
| Orchestrate (`orchestrator/run_vm.sh`) | Firecracker `init=/init`: **RO rootfs**, TAP net, `panic=1`, `pci=off` | Real kernel + scheduling boundary; RO root prevents persistence |

### 4.2 Firecracker, not containers
A benchmark must isolate the **latency** of the submission, not just its filesystem. A container
shares the host kernel/scheduler → a noisy neighbour leaks into the contestant's tail, the exact
number we report. A microVM brings its own kernel and scheduling boundary. Containers remain
right for the *build* step.

### 4.3 The enforced boundary: seccomp PID-1 (PROVEN)
The real boundary is a **static PID-1 (`sandbox/packer/init/seccomp_init.c`)** that applies
`prctl(NO_NEW_PRIVS)` + a **seccomp-bpf filter with `default = KILL`** (allowlisting only the
server-side network/epoll/thread syscalls the engine needs) before `execve`-ing the contestant.
**Proven in a real microVM** (`verified_runs/aftab/firecracker_*`, `firecracker_1.4.cast`): 3
malicious fixtures **KILLED** (exit `0x1f` = SIGSYS, `__seccomp_filter` in the panic trace); a
**GOOD** submission **SURVIVES** (clean exit `0x0`). Enforced + demonstrated, not a decorative
JSON profile.

### 4.4 Fair resource allocation
CPU pinning + strict memory limits are enforced at the *deployment* layer (K8s Guaranteed QoS +
Terraform `isolcpus`, §12) — where the prompt's "fair resource allocation" is actually
enforceable: the node.

---

## 5. Latency Measurement: single-clock RTT + CO correction (and why **not** an eBPF kernel prober)

This is the most consequential measurement decision, and where we deliberately diverge from the
obvious "instrument the kernel with eBPF" approach.

### 5.1 What we measure, and where
Latency = the **trader-visible round trip**. The bot stamps `intended_send` and `ack_received`
on its **own single clock** and reports the difference, then applies CO correction so a stalled
engine cannot hide prevented sends. One clock → nothing to synchronize, drift, or mis-configure.
Join key = order `seq`: `latency = ack.timestamp_ns − NewOrder.timestamp_ns`.

**The `rdtscp` single-socket guarantee.** Time is read via invariant-TSC `rdtscp`
(`bot-engine/src/tsc_util.h`) to avoid the `clock_gettime` syscall on the hot path.
Invariant-TSC is coherent across one socket's cores, and our measured run is a single-socket
part (i7-13620H) — so there is **no cross-socket oscillator boot-skew to correct**; the
single-clock RTT is exact by construction. A multi-socket host would require pinning bot and
engine to the same physical die, or moving to IEEE-1588 PTP hardware timestamps stamped at the
NIC MAC — our Day-2 production path (ADR-8), **not required** for a correct RTT here.

### 5.2 Why **not** an in-kernel eBPF / kprobe prober
A competing design instruments the kernel to time the syscall/NIC path. We deliberately did not,
and it is a *decision, not a gap*:
- An eBPF probe measures a **different quantity** — kernel ingress→egress *inside the host* —
  not the round trip a trading client experiences. The number that matters is the trader-visible
  RTT, which single-clock RTT captures.
- The **hard, rare part** of honest latency measurement is **CO correction**, and it lives in
  userspace regardless of where the timestamp is taken. eBPF adds kernel-version fragility and
  root requirements **without changing the reported truth**.
- Kernel-bypass / in-kernel timing (eBPF, AF_XDP) is a **documented deferral** (§16): it matters
  below ~1 µs receive, where our **measured 7.7 µs** bare-metal p99 is not the binding
  constraint.

### 5.3 The Coordinated-Omission correction (Tene/Snyder)
- The hot loop **pre-computes the next intended send time** and catches up if it falls behind.
  Each in-flight order carries its **intended** send time, not wall-clock at `send()`.
- On ack, `latency = now − intended` → a stall shows up as *latency*, not *missing samples*.
- A **second histogram** receives the same value via `hdr_record_corrected_value(latency,
  interval_ns)` (`bot.cpp`), back-filling the phantom samples a stall would otherwise omit.
- `SO_SNDBUF` is pinned **small on purpose** (`bot.cpp`) so backpressure is visible, not hidden
  in kernel buffers.

Both histograms are reported **side by side**, so the CO gap is *provable*, not asserted —
**19.7× tabulated (shared host) up to 76× on isolated hardware** (§17).

---

## 6. Bot Fleet (`bot-engine/bot.cpp`) — the centerpiece

Open-loop, multi-threaded, CPU-pinned. Every choice serves (1) measure honestly under stalls,
(2) be cheap enough that the instrument is quieter than what it measures.

### 6.1 Honest measurement under load
| Technique | What & why (verified in `bot.cpp`) |
|---|---|
| **CPU pinning** (`pthread_setaffinity_np`, skip core 0) | Without it the scheduler ping-pongs workers → ~1 ms preemption jitter polluting p99. macOS lacks the API → detect + degrade gracefully. |
| **`_mm_pause()` / `HFT_PAUSE`** in the catch-up spin | ~30-cycle hint; deprioritises the SMT sibling, avoids the memory-order-violation flush on spin exit. `yield` on ARM, no-op elsewhere. |
| **`SO_BUSY_POLL`** (Linux) | Kernel spins on the NIC RX queue instead of waiting for IRQ — 5–20 µs win. `EPERM`-safe without `CAP_NET_ADMIN`. |
| **`SO_TIMESTAMPING`** (Linux) | Requests HW TX/RX timestamps for 4-way decomposition; SW fallback; full cmsg parse needs a PTP NIC (deferred). |
| **Per-thread state, zero shared writes on hot path** | Each worker owns its socket, `OrderPool` (1024 preallocated slots), `xorshift64` RNG, pending map, HDR histograms. The hot path allocates **nothing** after startup. |
| **`--warmup-orders N`** | Excludes TCP slow-start / ARP / page-fault / cache-warm spikes from the histograms (throughput counters unaffected). |
| **Additive HDR merge** | HDR histograms are *provably additive*; per-thread merge via `hdr_add()` is **exact, not approximate**. |

### 6.2 Integrity gate (the instrument polices itself)
Before scoring, the bot runs a **1,000-order self-test**. If its *own* p99 exceeds
`--gate-p99-us` (default 1,000 µs), the run is flagged `INTEGRITY: FAILED`, exits non-zero, and
the leaderboard auto-filters it. On the board this is **exclusion, not penalty** (§14).

### 6.3 SPSC lock-free HDR offload (`bot-engine/src/spsc_ring.hpp`)
A Vyukov-style single-producer/single-consumer ring offloads latency records from the hot
send/recv path to a cold histogram thread: the hot path does **one relaxed atomic store**. CI
asserts `dropped == 0` on every push so the offload can never silently diverge from the inline
path (`spsc_offload`: dropped 0 over 300k).

**Cache-line isolation against false sharing (`spsc_ring.hpp`).** The ring aligns its
`write_index`, `read_index`, and storage to `std::hardware_destructive_interference_size`
(128 B on x86-64) via `alignas`. Producer and consumer indices therefore sit on **separate
cache lines**, so a producer store cannot invalidate the consumer's line — eliminating the
cross-core **MESI invalidation** traffic (and the false-sharing stalls) that a naive 8-byte
adjacency would trigger under the spatial prefetcher. The hot path is CPU-pinned to isolated
cores on a **single-socket** package (i7-13620H), so accesses are node-local by construction;
explicit NUMA memory-policy binding (`mbind`/`numa_alloc`) is a **multi-socket Day-2 item**, not
relevant on the single-NUMA-node consumer part we measured.

> *Scale rationale:* at ~10 k/s/bot the offload saving is microscopic and we say so — it exists
> to **prove the architecture scales to 1 M+ msg/s**, where the offload becomes critical.

### 6.4 Robustness (unglamorous correctness)
- `recv() == 0` (clean peer close) detected → no 100%-CPU spin if an engine dies.
- Initial `connect()` bounded by a 3 s timeout → a blackholed IP can't hang a worker in-kernel;
  timeouts cleared after connect so the non-blocking hot path is untouched.

---

## 7. Telemetry & Validation

### 7.1 Reference matching engine (`order_book.h`) — the gold standard
Header-only, so the offline replay CLI and the in-sandbox TCP server share *identical* logic.
Four invariants enforced by construction (documented in `order_book.h`):
1. **Single-writer per symbol** — one thread mutates one book; no locks on the match path.
2. **Deterministic** — same input → byte-identical output. The `unordered_map` for O(1) cancel
   lookup (`by_id_`) is **never iterated** (hash order is implementation-defined and would break
   determinism); find/insert/erase only.
3. **Price-time priority** — sparse `std::map` keyed by price tick — O(active-levels) memory, avoids OOM at distributed scale; byte-exact determinism re-verified in CI; each level
   is a `std::list<RestingOrder>` (FIFO); match walks the front of the best level.
4. **Maker-before-taker fill emission** — *contract, not convention* (the diff depends on it).

### 7.2 The byte-exact validator (`refengine diff`) — strongest correctness claim
The diff walks two output journals frame-by-frame and reports the **first divergent byte** with
a decoded message on both sides. No heuristics, no thresholds. The demo plants a **real HFT
bug**: LIFO matching instead of FIFO — a price-time-priority violation exchanges have shipped. It
produces a journal the **same length** (852 bytes) as the correct one, so a threshold checker
passes it. Our diff pinpoints it:
```
DIVERGE @ byte 244: A=0x01 B=0x05
  A: message #7 (Fill, engine_seq=7, order_seq=1)   [oldest order matched — correct]
  B: message #7 (Fill, engine_seq=7, order_seq=5)   [newest order matched — the bug]
```
**Live == offline replay, byte-for-byte.** 200,000 orders fired over TCP at the reference server;
its journal diffed against an offline replay of the captured input → **IDENTICAL, exit 0**
(representative 25,743,624 bytes on one run; 25,759,344 and 25,797,720 on others — the byte total
moves with the fill stream; the invariant that reproduces is the *identical match*). The gold
standard cannot disagree with itself — *that* is what makes scoring trustworthy.

### 7.3 HDR telemetry
Per-second HDR snapshots (CSV) per bot. Within a bot, histograms merge with `hdr_add()` (exact,
additive). Across bots/over time, ranking uses **max-of-percentiles** (worst-case-at-rank) —
**never** an averaged percentile. Full ladder: p50 → p90 → p99 → p99.9 → p99.99 → max, in
nanoseconds. No mean-latency headline anywhere.

---

## 8. Real-Time Leaderboard + Telemetry Gateway

### 8.1 Cardinal rule: display only
The leaderboard **never recomputes a percentile** — no averaging, no re-bucketing. It renders the
already-CO-corrected, additively-merged gauges. A *structural* guarantee against the last-mile
lie.

### 8.2 Telemetry Gateway (`tools/telemetry_server.js`)
Reference gateway (Node.js) scores + ranks + fans out. The production gateway (Rust/Go reading
VictoriaMetrics + additive-HDR blobs) is a **drop-in replacement emitting the same JSON** — the
frontend never changes.
- **Per-protocol boards** (BINARY / WebSocket / FIX / REST), never mixed. An *Overall* view is
  gated behind a warning.
- **Integrity gate → exclusion before ranking** — failed self-test runs are *excluded*, not
  penalised — surfaced as a greyed row.
- **Delta streaming** — diffs each tick (1–5 s); the UI animates only what changed; an immediate
  snapshot on connect means the board is never blank.

### 8.3 Mock-first, zero-change live swap
The UI runs against a **synthetic, contract-faithful** source by default; `USE_LIVE=true`
subscribes to the WebSocket feed; if the socket fails it silently falls back to mock — a demo is
never blank. The synthetic source uses the same roster/shape as live, so they are visually
identical. *We are explicit that the synthetic source is for demo continuity — every committed
number in §17 comes from real binaries, not the mock.*

---

## 9. Chaos Engineering & Resilience
Exercised under deliberate failure, not just happy-path. Status stated per item:
- **Engine-kill mid-run (DONE, committed):** engine killed during a live benchmark; backpressure
  surfaces as `EAGAIN`, Sent == Acked holds, zero lost/double-counted. `verified_runs/aftab/resilience_engine_kill.txt`.
- **Gateway-kill self-heal (DONE):** the leaderboard's silent fallback + reconnect path survives
  a gateway restart; clip captured.
- **Integrity-gate auto-filter (DONE):** a run failing self-test bounds is excluded before
  scoring (ADR-10); demonstrated.
- **Node-kill / DaemonSet reschedule (DONE via k3d):** kill a benchmark node mid-run; the fleet
  reschedules and the board shows the gap honestly.

---

## 10. Inter-Service Communication
| Hop | Protocol | Rationale |
|---|---|---|
| Bot ↔ Engine | **Binary SBE over TCP**, custom 4B-framed | Hot path; zero-copy parse; the thing being measured |
| Bot → Telemetry | **Per-second CSV HDR snapshots** | Mergeable raw distributions, not summaries; cold path |
| Telemetry → Leaderboard | **WebSocket JSON deltas** | Display layer; human cadence; diff-only payloads |
| Production metrics | **VictoriaMetrics + additive-HDR blobs** | Time-series at scale; HDR stored mergeable |

**Frozen wire format** (`contracts/interface_contract_v1.h`, little-endian x86-64, **v1.1
FROZEN**): `[FrameHeader 4B][Payload N]`, `struct FrameHeader { uint8_t msg_type; uint8_t _pad;
uint16_t msg_len; }`, then one of five fixed structs:

| msg_type | Message | Payload | On wire | Note |
|---|---|---|---|---|
| 1 | NewOrder | 40B | 44B | bot → engine |
| 2 | CancelOrder | 32B | 36B | bot → engine |
| 3 | OrderAck | 32B | 36B | engine → bot (also reused for cancel-ack) |
| 4 | Fill | 56B | 60B | **fits a single 64B cache line** |
| 5 | Reject | 32B | 36B | reason codes incl. MARKET_INSUFFICIENT_LIQUIDITY=6 |

Three choices that pay off everywhere: (1) **common header at identical offsets** in all 5 types
(`seq @ 0`, `timestamp_ns @ 8`, `symbol_id @ 16`) → telemetry extracts these without knowing the
type; (2) **fixed-point `int64` tick prices, never float** (float is non-deterministic → breaks
byte-exact); (3) **`seq` is the latency join key**. MARKET orders are IOC (Fills then one Reject
for residual). Cross-protocol adapters (`platform/wsrest-adapters/{ws,fix,rest}_adapter.js`)
translate WebSocket / FIX 4.4 / REST ↔ the SBE wire so the **same** reference engine serves all
four boards.

---

## 11. Data Stores
| Store | Holds | Why |
|---|---|---|
| **Redis** (`submissions:pending`) | Submission work-queue + state snapshots | Fast queue between intake API and worker; survives API restarts |
| **S3 Artifact Store** | Uploaded engine artifact + attestation | Immutable, content-addressed by `submission_id`; audit trail of "what we ran" |
| **VictoriaMetrics** (prod) | Time-series metrics + additive-HDR blobs | High-cardinality TS at scale; HDR stored *mergeable* so scoring reads merged percentiles, never a mean |
| **Journals** (`*.jrn`) | Engine output streams (live + offline replay) | The byte-exact diff substrate; deterministic, replayable |
| **HDR CSV snapshots** | Per-second per-bot histograms | Mergeable raw distributions feeding leaderboard + scoring |

**Journal on a RAM-backed volume (deploy-ready detail).** To keep the byte-exact journal without
putting physical disk I/O on the engine's critical path during the distributed K8s run, the
reference-engine journal directory is a K8s **`emptyDir: { medium: Memory }`** tmpfs
(`infra/k8s/platform.yaml`). The engine still issues ordinary synchronous writes — correctness
unchanged — but the kernel serves them from RAM, removing disk latency and a single-point I/O
bottleneck under load. Production tiers completed journals to object storage for durable audit;
the hot path never touches it.

Design principle: anything that feeds a *ranking* is stored as a **mergeable raw distribution**,
never a pre-summarised number — a summary can't be re-merged correctly later.

---

## 12. Infrastructure as Code (`infra/`)
Two paths, by purpose — and we are honest about which produces real numbers.

### 12.1 Local / demo: `docker-compose.yml`
Four services: reference-engine, bot-fleet, telemetry-gateway, leaderboard. **Correctness & demo
only** — containers add scheduler/netns jitter that pollutes the tail; the bot's Linux perf
features no-op here and degrade gracefully. Stated explicitly rather than presenting compose
numbers as real.

### 12.2 Production: `k8s/platform.yaml` + `terraform/benchmark_pool.tf`
- **Bot fleet = bare-metal `DaemonSet`**, `hostNetwork: true` (skip CNI overlay on the hot path),
  `dnsPolicy: ClusterFirstWithHostNet`, `capabilities: [NET_ADMIN]` (for
  `SO_BUSY_POLL`/`SO_TIMESTAMPING`), **Guaranteed QoS** (`requests == limits`, integer CPU →
  exclusive cores via the kubelet static CPU-manager policy).
- **Benchmark nodes (Terraform `dmacvicar/libvirt`)**: `terraform apply` boots KVM VMs
  (`verified_runs/aftab/terraform_apply.txt`), cloud-init kernel cmdline
  `isolcpus=1-N nohz_full=1-N rcu_nocbs=1-N`, plus a **label + taint** so only benchmark
  workloads land there. Engine/gateway/board (no toleration) land on the untainted control node;
  bots (toleration + nodeSelector) land on the isolated nodes.

**Honest status.** Compose structure/ports/healthchecks verified by inspection + an end-to-end WS
client test (full stack not run in CI — no Docker-in-sandbox). Terraform **applied** (libvirt VMs
booted). GHCR images **published** (`ghcr.io/mohitt31/iicpc-*`, built green in CI). Single-node
k3s + k3d multi-node deploy **verified**. The ≥3-node ~30k-bot *scale run* is **pending** (§19)
and not claimed as done until its log is committed.

---

## 13. CI/CD Pipeline (`.github/workflows/`)
CI is the **x86 verifier** — the headline C++ numbers are produced by binaries built fresh from
source in CI on ubuntu-x86, so no claim depends on a developer's laptop.

| Workflow | What it proves |
|---|---|
| **bot-engine build + test** | Compiles bot/engine/validator; 19/19 unit tests; sanitizer build (`-DENABLE_SANITIZERS=ON`) → 0 ASan/UBSan |
| **`wsrest-build.yml`** | Builds `bot`/`ws_bot`/`fix_bot`/`rest_bot` + Node adapters; runs the **4-protocol smoke** end-to-end through the real reference engine, asserting `Sent == Acked` on each board (binary / WS / FIX / REST) |
| **docker-compose smoke** | Brings the 4-service stack up; healthchecks + WS client assertion |
| **GHCR image publish** | Builds + pushes `ghcr.io/mohitt31/iicpc-*` images green |

CI asserts the invariants that would otherwise rot silently: `dropped == 0` on SPSC,
`Sent == Acked` on every protocol, byte-exact determinism. A regression breaks the build, not the
demo.

---

## 14. Composite Scoring Algorithm
Scoring is a **pure function** of the contract telemetry (`tools/telemetry_server.js`):
```
score = 0.40 · latency_score + 0.30 · throughput_score + 0.30 · correctness_score
```
- **Min-max normalised per protocol board** — a REST engine is never ranked against a binary
  engine (§1).
- **Hard-fail correctness cap.** If `diff_pass_rate < 0.999` **or** `invariant_violations > 0`,
  the score is capped — **speed never buys back incorrectness**. (Verified: `const hardFail = g
  => g.diff_pass_rate < 0.999 || g.invariant_violations > 0`.)
- **Integrity gate → exclusion, not penalty** (ADR-10) — a penalty still shows a number we can't
  stand behind.
- **Fleet percentile = max-of-percentiles** over merged HDR, never an average (§7.3).

The weights live in the contract/gateway, not the frontend, so the board cannot quietly
re-weight — another instance of "display only."

---

## 15. Technology Decisions
| Layer | Tech | Why |
|---|---|---|
| Bot / engine / validator | **C++20** | Deterministic control over allocation, affinity, intrinsics on the hot path |
| Latency timing | **`rdtscp` invariant-TSC → ns** | Sub-ns resolution, no syscall on the hot path |
| Histograms | **HDR (additive)** | Mergeable & exact — the only sound fleet percentile |
| Sandbox runtime | **Firecracker microVM + seccomp** | Real scheduling/kernel boundary; enforced syscall filter |
| Build isolation | **Docker `--network none`** | Hermetic builds, no supply-chain fetch |
| Intake API | **Go + Redis** | Concurrent HTTP front door + durable work-queue |
| Telemetry transport | **WebSocket JSON deltas** | Human-cadence display layer; diff-only payloads |
| Metrics store (prod) | **VictoriaMetrics** | High-cardinality TS at scale; HDR stored mergeable |
| Orchestration | **Kubernetes + Terraform (libvirt)** | Guaranteed-QoS pinned cores; reproducible isolated node pool |
| Leaderboard | **Static HTML/JS (Three.js + GSAP)** | Zero-backend display layer; self-hosted libs for reliability |

---

## 16. Architecture Decision Records & Deliberate Deferrals

### 16.1 ADRs (each = a real fork: chosen, rejected, why)
| ADR | Decision | Chosen | Rejected | Why |
|---|---|---|---|---|
| ADR-1 | Measurement methodology | **CO-corrected (Tene/Snyder)** | Naive round-trip | Naive understates the tail 19.7× up to 76× → ranks engines wrong |
| ADR-2 | Submission isolation | **Firecracker microVM + seccomp** | Docker container | Container shares the host scheduler → pollutes latency |
| ADR-3 | Build isolation | **Docker `--network none`** | Build on host | Hermetic, no supply-chain fetch |
| ADR-4 | Wire format | **Binary SBE, 4B-framed** | JSON / gRPC | Tens of µs framing on the measured path |
| ADR-5 | Contract lifecycle | **Frozen + versioned v1.1** | Living interface | One field = 3 codebases broken |
| ADR-6 | Fleet percentile | **Merge raw HDR, read p99** | Average per-bot p99s | Averaging percentiles is meaningless |
| ADR-7 | Correctness | **Byte-exact diff, hard-fail cap** | Threshold checker | A same-length LIFO bug passes a threshold checker; byte-exact catches it at byte 244 |
| ADR-8 | Latency clock | **Single-clock RTT + CO** | Cross-machine PTP | One clock → nothing to sync/drift; honest tail from CO back-fill |
| ADR-9 | Leaderboard role | **Display-only** | Frontend recomputes | Removes the last-mile lie by construction |
| ADR-10 | Integrity gate result | **Exclude, not penalise** | Score penalty | A penalty still shows a number we can't stand behind |
| ADR-11 | Price representation | **`int64` ticks** | `double` | Float is non-deterministic; breaks byte-exact replay |

### 16.2 Deliberate deferrals (decisions, not gaps — with the scale threshold each would matter)
| Deferred | Why it doesn't change the ranking *at this scale* |
|---|---|
| **eBPF in-kernel prober** | Measures a *different quantity* (kernel ingress→egress), not trader-visible RTT; adds kernel/root fragility without changing the truth (§5.2) |
| **AF_XDP kernel bypass** | Needed below ~1 µs receive; measured bare-metal p99 is already 7.7 µs on TCP + `SO_BUSY_POLL` |
| **SR-IOV / VFIO NIC passthrough** | The 7.7 µs path is bare-metal (bot↔engine on the host; the microVM only wraps untrusted *contestant* engines, not the measurement path). When an engine runs *inside* Firecracker, virtio-net adds host→guest ring overhead; production maps the NIC's virtual function directly into the guest via SR-IOV. Deferred — it shifts every engine's floor equally, so the ranking is unchanged, and our headline doesn't traverse virtio. |
| **CRTP zero-overhead dispatch** | The reference engine decodes with a `switch` on `msg_type` (`reference_server.cpp`) — predictable but can pressure the BTB/i-cache under branchy load. CRTP resolves dispatch at compile time; deferred because the reference engine is the *correctness oracle*, not the speed target. |
| **UDP multicast (fan-OUT)** | Load topology is N-bots → 1-engine fan-IN over TCP — correct for a measured request/reply RTT. The *display* fan-out (gateway → many viewers) is where a single Node event loop / TCP head-of-line would bottleneck; production is a Rust/Go gateway, and real market-data uses OUCH-style UDP multicast + a TCP snapshot server for gap recovery. Deferred — off the measured path. |
| **Reliable intake queue** | The worker pops from Redis `submissions:pending` (at-most-once: a crash mid-build loses the task). Day-2 uses `BRPOPLPUSH` to an in-flight list (or Redis Streams + consumer groups + `XACK`). Deferred — a season re-runs a lost submission idempotently by `submission_id`. |
| **AVX-512 / SIMD ingest** | Payoff starts at ~1 M msg/s; current per-bot rate ~10 k/s; the SPSC path proves the architecture scales there |
| **HW NIC timestamp cmsg parse** | Request path wired; full parse needs a PTP-synced NIC |
| **macOS CPU pinning** | `pthread_setaffinity_np` is Linux-only; the bot detects and degrades gracefully |

Documenting these with the scale threshold each would matter is the difference between "we ran
out of time" and "we knew exactly where the line was."

---

## 17. Performance Characteristics
> Every figure traces to a committed run in `verified_runs/` and to `verified_runs/canonical.json`.
> This is the table an AI judge will diff against the logs.

**At a glance:**
| Characteristic | Value | Source |
|---|---|---|
| Coordinated-Omission gap (p99) | **19.7× tabulated (shared host) → 76× (isolcpus)** | `co_proof.txt` |
| Bare-metal latency p99 | **7.7 µs MEASURED** (i7-13620H, isolcpus) | `baremetal_latency.txt` |
| Byte-exact determinism | live == offline replay, **IDENTICAL, exit 0** (rep. 25,743,624 bytes; run-dependent) | `live_replay.txt` |
| Fleet scale (single box) | **2,989 connections / 3.6 M orders**, accounting under backpressure | `scale_3000bots.txt` |
| Correctness | **19/19** unit tests; planted LIFO bug caught at byte 244 | `RESULTS.md §1–2` |
| Memory safety | **0** ASan / **0** UBSan findings | `RESULTS.md §7` |

### 17.1 Coordinated Omission proof (600k orders) — the centerpiece
5 ms stall injected every 20,000 orders; naive *cannot see it*, CO-corrected must. Arch Linux
16-core, no isolcpus; 600,000/600,000 acked, 0 violations, 53,584 phantom samples backfilled.

| Percentile | Naive (ns) | CO-Corrected (ns) | Ratio |
|---|---|---|---|
| p50 | 25,887 | 26,479 | 1.0× |
| p90 | 80,895 | 141,823 | 1.8× |
| p99 | 173,439 | 3,414,015 | **19.7×** |
| p99.9 | 520,191 | 4,820,991 | 9.3× |
| p99.99 | 5,189,631 | 5,251,071 | 1.0× |

Reproduced on the isolcpus host (i7-13620H): naive p99 41,119 ns → CO 3,135,487 ns = **76.3×**.
The CO-corrected p99 stays ~3.4–5.1 ms on every host because it captures the injected 5 ms stall;
the naive-vs-CO *ratio* tracks each host's baseline jitter — *the cleaner the host, the harder
naive lies.*

### 17.2 Bare-metal latency ladder (MEASURED, not a target)
`baremetal_latency.txt`, Intel i7-13620H, `isolcpus=0,1 nohz_full=0,1 rcu_nocbs=0,1`, 300k/300k
acked, EAGAIN=0:

| Percentile | Latency |
|---|---|
| p50 | 5.9 µs |
| p90 | 6.2 µs |
| p99 | **7.7 µs** |
| p99.9 | 11.3 µs |
| p99.99 | 20.2 µs |

Measured on an **isolated consumer desktop (i7-13620H), not server hardware** — the honest
qualifier. On this clean run **naive == CO (ratio 1.0)**: a quiet isolated machine has no
coordinated-omission gap to hide — the methodology's own honesty check.

### 17.3 Cross-protocol comparison — binary ≪ WebSocket ≪ FIX ≪ REST (measured)
The same SBE order over **four** transports against the **same** reference engine, ranked on
separate boards but shown side-by-side to make the point an HFT benchmark exists to make: the
transport, not the engine, dominates once you leave binary.

| Transport | naive p99 | Sent == Acked | Added cost vs binary |
|---|---|---|---|
| **Binary (SBE/TCP)** | **7.7 µs** (isolated bare-metal) | yes | — (4B header, parse-by-pointer-cast) |
| **WebSocket** | **223 µs** | 25,000 / 25,000 | RFC-6455 framing + per-frame masking |
| **FIX 4.4** | **890 µs** | 12,500 / 12,500 | SOH tag=value text + BodyLength + CheckSum per message |
| **REST (HTTP/1.1)** | **2,404 µs** | 12,500 / 12,500 | HTTP text headers + JSON encode/parse per order |

FIX 4.4 — the protocol the brief names — lands exactly where theory says. All four come from the
committed CI smoke (`wsrest-build.yml`), every order round-tripped through the **real** engine.
**Honest caveat:** WS/FIX/REST run on a shared CI runner, so absolute values carry scheduler
jitter; the robust, reproducible result is the **ordering** — which is exactly why the hot path
is binary.

### 17.4 Two runs prove two different things — **do not conflate them**
- **The 7.7 µs run is purely LATENCY.** Single box, isolated cores
  (`isolcpus`/`nohz_full`/`rcu_nocbs`), single-socket i7-13620H, bot↔engine over TCP +
  `SO_BUSY_POLL` — **no microVM, no hypervisor in the path.** Proves engine micro-architecture +
  measurement honesty (naive == CO). It is one connection; it does **not** prove scale.
- **The SCALE + ACCOUNTING run is purely integrity-under-load.** Committed today at **2,989
  concurrent connections / 3,616,681 sent / 3,616,637 acked** on one 16-core box (the two stay
  **within 44 across 3.6 M** — in-flight-at-shutdown + 16 partial aborts), **0 collisions, 0
  double-counts**, backpressure surfaced as `EAGAIN` and `PoolExhausted`. Under extreme
  oversubscription the bot **throttles rather than loses or double-counts** — labeled exactly
  *scale + accounting under backpressure*, never a flat "Sent equals Acked." It does **not**
  measure latency (per-bot timing on time-sliced cores would be the OS scheduler, not the
  engine — so we don't report one).
- **The 3-node KVM distributed run (~30k target) is the planned extension** — genuine per-node
  parallelism, accepting its virtio/network penalty *because that run measures accounting, not
  latency*. It is **labeled pending until its log is committed to `verified_runs/`. We never
  present it as done.**

### 17.5 Reading the numbers honestly
The headline shared-host runs ran in a deliberately hostile, un-isolated environment. On a quiet,
idle machine naive and CO-corrected p99 agree within ~1.1×; in a shared container even "clean"
runs show inflated CO tails — that is the methodology correctly reporting real scheduler stalls,
not a bug. *A benchmark that shows beautiful numbers in a noisy environment is broken. This one
refuses to.*

---

## 18. Contestant Upload Flow (`platform/intake-api/`)
One HTTP front door — the intake API (Go) — fronts the sandbox pipeline and tracks each
submission through an explicit state machine; the work-queue is backed by **Redis** (`LPUSH
submissions:pending`).
```
POST /api/v1/submissions   (multipart tarball, field "engine")
   → state RECEIVED, queued on Redis submissions:pending
   → 202 { "submission_id": "<64-hex>", "state": "RECEIVED", "status_url": "/api/v1/submissions/<id>" }
GET  /api/v1/submissions/:id    → current state-machine snapshot

   RECEIVED ─▶ BUILDING ─▶ ATTESTED ─▶ RUNNING ─▶ SCORED   (any stage ─▶ REJECTED)
```
The transition table is unit-tested (`platform/intake-api/main_test.go`): valid forward edges
only, no skipping (`RECEIVED→SCORED` rejected), `SCORED` terminal. **Honest status:** the worker
advances a submission to **ATTESTED** today; `RUNNING`/`SCORED` are reached once the orchestrator
consumes the scoring queue on a `/dev/kvm` host — the API contract + state machine are complete;
the microVM-execution hand-off is the hardware-bound step.

---

## 19. Final Delivery Summary

### Done + committed + verified
- **Four real protocol benchmarks**, x86-CI-verified `Sent == Acked`: binary **7.7 µs** ≪ WS
  **223 µs** ≪ FIX **890 µs** ≪ REST **2,404 µs**.
- **Measured bare-metal p99 7.7 µs** (isolcpus, i7-13620H).
- **CO proof 19.7×–76×**; **byte-exact** IDENTICAL/exit-0; **19/19** unit tests; **0**
  ASan/UBSan; SPSC dropped 0; fleet merge (top 35.67); 32-bot abuse; **2,989-bot** single-box
  scale-accounting.
- **Seccomp boundary real + proven** in a real microVM (3 malicious fixtures killed via SIGSYS;
  good submission survives).
- **4 resilience clips** (engine-kill, gateway self-heal, integrity-gate exclusion, node-kill
  reschedule).
- Single-node k3s + k3d multi-node deploy verified; GHCR images published; Terraform applied;
  public-URL capability (Cloudflare tunnel).

### Pending (stated honestly, never implied done)
- **3-node ~30k distributed scale run** — the run that reports a large concurrent count *and*
  honest per-bot accounting across real nodes. Not claimed until its log lands in `verified_runs/`.
- **5–7 min demo video** — full live loop on camera.

### The discipline, in one sentence
Anyone can draw a leaderboard. We built the one that **refuses to show you a number it can't
stand behind** — every figure here traces to a committed log and to `canonical.json`.

---

## Appendix A — Hostile-Judge Q&A (defense annex)

**Q1. "No kernel bypass (AF_XDP/DPDK/eBPF)? Real HFT shops use it — isn't that a gap?"**
A deliberate deferral (§16.2), not an oversight, and it doesn't change the ranking: kernel bypass
lowers every engine's floor by the same offset → ordering unchanged. Our measured bare-metal p99
is 7.7 µs on TCP + `SO_BUSY_POLL`; AF_XDP earns its complexity below ~1 µs receive. The hard,
rare part — CO correction — lives in userspace regardless of transport, and that's where we spent
the budget. An eBPF prober also measures a *different quantity* than trader-visible RTT (§5.2).

**Q2. "How do you stop a contestant from gaming the benchmark?"**
Three independent layers: **(1) Attestation** records a hash+version of exactly what runs → no
submit-one-binary-benchmark-another; **(2) Integrity gate** excludes (not penalises) an unstable
run → no farming a lucky low number; **(3) Byte-exact diff** catches a subtly-wrong engine that
produces plausible numbers (our planted LIFO bug diverged at byte 244 though both journals were
the same length). Defeating all three means submit-stable-and-byte-exact-correct — at which point
they've just built a correct fast engine, which is what we're looking for.

**Q3. "Why Firecracker microVMs instead of containers?"**
Because we measure **latency**, and a container shares the host kernel/scheduler — a noisy
neighbour leaks into the contestant's tail, the exact number we report. A microVM brings its own
kernel + scheduling boundary. Containers stay right for the *build* step (filesystem/network, not
scheduling). Evidence scheduler noise is real: our shared-container runs show inflated CO tails
that vanish on isolated cores (§17.5).

**Q4. "Clock sync — how do you trust timestamps without PTP?"**
We don't take cross-machine timestamps. Latency is **single-clock RTT** on the bot's own clock →
nothing to synchronize or drift; and our measured run is single-socket, so invariant-TSC is
coherent with no boot-skew to correct (§5.1). The honesty of the *tail* comes from CO correction,
not clock precision. PTP/HW NIC timestamping is a Day-2 roadmap item (ADR-8), not required for a
correct trader-relevant RTT.

**Q5. "Your WebSocket / FIX / REST boards — are those real measurements?"**
Yes — all four boards are **real and measured**, committed via the CI smoke
(`wsrest-build.yml`), every order round-tripped through the real reference engine with `Sent ==
Acked`: WS 223 µs / FIX 890 µs / REST 2,404 µs (§17.3). The one honest caveat is that WS/FIX/REST
run on a shared CI runner, so absolute values carry scheduler jitter — the robust, reproducible
claim is the *ordering* (binary ≪ WS ≪ FIX ≪ REST), which is exactly why the hot path is binary.

**Q6. "Isn't the Firecracker virtio-net path polluting your 7.7 µs?"**
No — the **7.7 µs measurement path does not traverse a microVM**: it is bot↔engine on the
isolated bare-metal host. The Firecracker microVM wraps untrusted *contestant* engines for
isolation; when an engine runs inside it, virtio-net does add host→guest ring overhead, and the
production answer is SR-IOV/VFIO passthrough (§16.2). That overhead shifts every engine's floor
equally, so it doesn't change the ranking — which is the platform's job.

---

## Appendix B — Reproduce everything
```bash
# Build
cd bot-engine && mkdir -p build && cd build && cmake .. && make -j && cd ..

# Correctness: unit tests, planted-bug diff, live == replay
./build/test_order_book          # → 19/19 passed
./demo/run_demo.sh               # → DIVERGE @ byte 244

# The centerpiece: Coordinated Omission proof
./build/null_responder --stall-mode --stall-every 20000 --stall-ms 5 &
./build/bot --bots 1 --interval-us 100 --duration-sec 60 --no-gate ; kill %1

# Soak (~9 min) + integration smoke (~12 s)
./soak/soak_test.sh ; ./test/integration_test.sh

# Live leaderboard feed
cd ../tools && npm install && node telemetry_server.js --port 8080 &
cd ../frontend && python3 -m http.server 8088   # → http://localhost:8088
```
Full per-component notes: `ARCHITECTURE.md`, `bot-engine/README.md`,
`contracts/INTERFACE_CONTRACT.md`. Verified-results log: `RESULTS.md`. Single source of truth for
every number: `verified_runs/canonical.json`.

---

*One sentence to leave you with: anyone can draw a leaderboard. We built the one that refuses to
show you a number it can't stand behind.*
