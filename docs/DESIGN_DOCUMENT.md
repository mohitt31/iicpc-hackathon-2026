# IICPC Summer Hackathon 2026: Design Document

**Project:** A Distributed Benchmarking & Hosting Platform for Trading Infrastructure
**Team Name:** IICPC HFT Benchmarking Platform

> **Demo Video Link:** `[Insert YouTube Link Here]`

---

## 0. How to read this document
This document serves as both the High-Level Design (HLD) and the Low-Level Design (LLD). It is organized so judges can evaluate it directly against the stated rubric:
We optimized for **depth over breadth**: rather than four half-built components, we built three that are mathematically sound and correct. We made the one thing most benchmarks get wrong—**Coordinated Omission**—the centerpiece of our design.

## Table of Contents
1. [Executive Summary](#1-executive-summary)
2. [Problem Analysis: What we actually understood](#2-problem-analysis-what-we-actually-understood)
3. [High-Level Design](#3-high-level-design)
4. [LLD: Component 1 - Submission & Sandboxing Engine](#4-lld-component-1---submission--sandboxing-engine)
5. [LLD: The Frozen Contract](#5-lld-the-frozen-contract)
6. [LLD: Component 2a - Reference Matching Engine](#6-lld-component-2a---reference-matching-engine)
7. [LLD: Component 2b - Bot Fleet + Telemetry (The Centerpiece)](#7-lld-component-2b---bot-fleet--telemetry-the-centerpiece)
8. [LLD: Component 3 - Real-Time Leaderboard](#8-lld-component-3---real-time-leaderboard)
9. [Decision Log: Alternatives & Trade-offs (Rubric #2 & #3)](#9-decision-log-alternatives--trade-offs-rubric-2--3)
10. [Deployment & Infrastructure as Code](#10-deployment--infrastructure-as-code)
11. [Verified Results (Rubric #6)](#11-verified-results-rubric-6)
12. [Technology Choices (and Why)](#12-technology-choices-and-why)
13. [Production Roadmap](#13-production-roadmap)

---

## 1. Executive Summary
We were asked to build a platform that lets contestants upload a matching engine, hosts it under isolation, bombards it with a distributed bot fleet, and ranks it live on latency, throughput, and correctness.

We built exactly that, as three independently-developed components bound by one frozen wire contract:
1. **Sandbox (`sandbox/`)**: A Firecracker-microVM pipeline that takes an untrusted submission and runs it with a real scheduling/kernel boundary. This ensures we benchmark the submission's actual latency, not host noise.
2. **Bot Fleet + Telemetry (`bot-engine/`)**: An open-loop, CPU-pinned, C++ load generator that measures tail latency honestly (Coordinated-Omission corrected). It also includes a gold-standard reference engine and a byte-exact correctness validator.
3. **Real-Time Leaderboard (`frontend/`)**: A display-only board that streams live deltas and never recomputes a percentile. It cannot lie because it only renders what the scoring service has already cryptographically proven.

The seam between them is `contracts/interface_contract_v1.h`—a frozen, versioned binary wire format. One field change breaks three codebases, so we froze it on day one. This architecture allowed three people to build in parallel without integration hell.

---

## 2. Problem Analysis: What we actually understood
Most teams will read the prompt as "build 4 boxes: upload, sandbox, load, chart." That interpretation fails to capture the true engineering challenge. Here is what the problem actually demands:

### 2.1 The benchmark's job is to be trustworthy, not pretty
A benchmarking platform produces rankings that decide a winner. If the numbers are wrong, the platform confidently rewards the wrong engine. Therefore, the dominant requirement is measurement integrity. Three integrity threats dominate:
- **Coordinated Omission (CO)**: A naive load generator sends an order, waits for the ack, records the round-trip, and sends the next. If the engine stalls for 5 ms, the bot also stalls. The samples that should have landed during those 5 ms are never taken. The histogram reports `p99 = 30 µs` while the real worst-case experience was 5 ms. **A platform that ranks engines on CO-blind numbers ranks them wrong.**
- **Instrument Jitter**: If the bot's own jitter is 1 ms, it cannot resolve a 50 µs engine. You must prove the instrument is quieter than the signal.
- **Correctness vs Performance**: An engine that violates price-time priority can appear faster because it skips fairness work. If you only measure latency, you reward cheating. Correctness must be a hard gate.

### 2.2 "Containerize the submission" is a trap
The prompt asked to "containerize" submissions for isolation. Containers isolate the filesystem, but they share the host scheduler and kernel. For a latency benchmark, that is fatal. A co-scheduled noisy neighbor will manifest as tail latency in the submission's score. **The correct isolation primitive for a latency benchmark is a microVM (Firecracker)** because it provides its own kernel and scheduling domain.

---

## 3. High-Level Design

```text
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

**End-to-end Data Flow:**
1. Contestant submits an engine. The **Sandbox** hashes it to an immutable `submission_id`, builds it hermetically, packs a rootfs, and boots it in a Firecracker microVM.
2. **Bot Fleet** drives constant-arrival-rate load at the engine, recording two HDR histograms (naive and CO-corrected), and running an integrity self-test first.
3. Per-second HDR snapshots and correctness diffs flow to the **Telemetry Gateway**, which scores and ranks reading `p99` from the merged additive HDR.
4. The gateway diffs each tick and fans out display-ready deltas over WebSocket to the **Leaderboard**.

---

## 4. LLD: Component 1 - Submission & Sandboxing Engine
**Goal:** Run an untrusted contestant engine such that we can benchmark its latency without trusting it and without polluting the measurement.

**Pipeline Stages (`sandbox/pipeline.sh`):**
1. **Intake:** Accepts a submission, hashes it to a 128-bit `submission_id`, and performs structural validation (e.g., verifying `syscall(2)` bounds).
2. **Builder:** Compiles/links inside a controlled environment using `-march=x86-64-v2` and `--network none` to ensure hermetic builds.
3. **Attester:** Records a cryptographic measurement of the binary to guarantee we benchmark exactly what was submitted.
4. **Packer:** Builds a minimal `ext4` rootfs containing the submission.
5. **Orchestrator:** Provisions a Firecracker microVM with `panic=1` and `pci=off`, sets up networking, runs the benchmark, and tears it down.

---

## 5. LLD: The Frozen Contract
The seam `contracts/interface_contract_v1.h` is a little-endian, x86-64 binary wire format.

**Wire format layout:** `[ FrameHeader 4B ][ Payload N bytes ]`
Every message type (`NewOrder`, `CancelOrder`, `OrderAck`, `Fill`, `Reject`) has a common header at identical offsets: `seq @ 0`, `timestamp_ns @ 8`, `symbol_id @ 16`. 
Telemetry extracts these three fields from any message without knowing its type, making the diff tool and latency join operation extremely cheap.

---

## 6. LLD: Component 2a - Reference Matching Engine
The gold standard every contestant is diffed against (`order_book.h`).

**Invariants:**
1. **Single-writer per symbol:** No locks on the match path.
2. **Deterministic:** We explicitly avoid iterating over `std::unordered_map` (used for O(1) cancel lookup via `by_id_`) because hash iteration order is implementation-defined and breaks determinism.
3. **Price-time priority:** Uses sparse `std::map` keyed by price tick — O(active-levels) memory, avoids OOM at distributed scale; byte-exact determinism re-verified in CI; each level is a `std::list<RestingOrder>` (FIFO).

**Byte-Exact Validator:**
The diff tool walks two output journals frame-by-frame and reports the first divergent byte. No heuristics. The provided demo plants a real-world HFT bug (LIFO matching instead of FIFO) and our validator instantly pinpoints it: `DIVERGE @ byte 244`.

---

## 7. LLD: Component 2b - Bot Fleet + Telemetry (The Centerpiece)
An open-loop, multi-threaded, CPU-pinned load generator (`bot.cpp`).

### 7.1 Coordinated Omission Correction
We implemented the structural Tene/Snyder fix:
- The hot loop pre-computes the next intended send time. Each in-flight order is tagged with its **intended** send time, not the wall-clock time at `send()`.
- On ack, `latency = now − intended`. A stall therefore shows up as latency, not as missing samples.
- A second histogram receives the same value via `hdr_record_corrected_value(latency, expected_interval)`, which backfills the phantom samples a stall would otherwise omit.
- We deliberately pin `SO_SNDBUF` small so backpressure is visible and not hidden in kernel buffers.

### 7.2 Integrity Gate
Before the scoring run, the bot runs a self-test against the target. If the bot's own `p99` exceeds `--gate-p99-us` (default 1,000 µs), the run is flagged `INTEGRITY: FAILED`. **The measuring instrument must beat what it measures.**

---

## 8. LLD: Component 3 - Real-Time Leaderboard
**Cardinal Rule: Display Only.**
The leaderboard never recomputes a percentile. It does not average or re-bucket. It only renders the contract's already-CO-corrected gauges. 

- **Full Percentile Ladder:** p50 to p90 to p99 to p99.9 to p99.99 to max. No mean-latency headline anywhere—averages hide the stalls that matter.
- **Composite Score:** `0.40*latency + 0.30*throughput + 0.30*correctness`. If `diff_pass_rate < 0.999` or `invariant_violations > 0`, the score is hard-capped. Speed never buys back incorrectness.

---

## 9. Decision Log: Alternatives & Trade-offs (Rubric #2 & #3)
Engineering is about trade-offs. Here are the paths we chose not to take, and why:

1. **MicroVMs (Firecracker) vs. Docker Containers**
   - *Alternative Considered:* Standard Docker containers for sandboxing.
   - *Trade-off:* Containers are easier to orchestrate, but they share the host kernel scheduler. A noisy neighbor process will pause the container, injecting artificial tail latency into the contestant's score.
   - *Decision:* We chose Firecracker MicroVMs. They require a heavier boot pipeline (rootfs packing), but they provide a hard scheduler boundary. This guarantees that latency spikes belong to the contestant's code, not our platform.

2. **Open-loop vs. Closed-loop Load Generation**
   - *Alternative Considered:* Closed-loop bot (wait for Ack before sending next Order).
   - *Trade-off:* Closed-loop is easier to code and requires no complex memory pools. However, it suffers from Coordinated Omission (CO).
   - *Decision:* We built an open-loop generator with pre-computed intended send times. It drastically increases the complexity of our state management (requiring lock-free SPSC queues and custom memory pools), but it is the *only* mathematically sound way to measure tail latency under stalls.

3. **Fixed-Point Arithmetic (Int64) vs. Floating Point**
   - *Alternative Considered:* Storing prices as `double` or `float`.
   - *Trade-off:* Floats natively handle decimal prices out of the box. However, floating-point math is notoriously non-deterministic across different CPUs, compilers, and optimization flags.
   - *Decision:* We enforced `int64` ticks in the binary wire contract. For a byte-exact correctness validator to work flawlessly, 100% determinism is required.

---

## 10. Deployment & Infrastructure as Code
- **Local/Demo (`infra/docker-compose.yml`)**: Correctness and demo only. Containers add scheduler jitter that pollutes the tail. We state this explicitly rather than presenting compose numbers as real.
- **Production (`infra/k8s/`, `infra/terraform/`)**: The bot fleet runs as a bare-metal `DaemonSet` with `hostNetwork: true` and `capabilities: [NET_ADMIN]`. Benchmark nodes are provisioned with `isolcpus=1-15 nohz_full=1-15 rcu_nocbs=1-15` via Terraform. This is where Linux-only latency features engage.

---

## 11. Verified Results (Rubric #6)
Every number is reproducible.
- **Correctness:** 19/19 unit tests passed. Our byte-exact validator accurately catches the planted LIFO bug.
- **Live TCP == Offline Replay:** 200,000 orders fired over TCP yield a journal identical to the offline replay: `IDENTICAL: 25,743,624 bytes match.`
- **CO Proof:** On a deterministic 5 ms stall every 20,000 orders, the naive p99 reads `173 µs` while our CO-corrected p99 reads `3.41 ms` - a 19.7x gap tabulated on a shared host, up to 76x on isolated hardware. The CO number is the truth. 
- **Memory Safety:** ASan + UBSan runs yielded 0 findings.

---

## 12. Technology Choices (and Why)
Every tool was chosen to preserve measurement integrity:

- **`rdtscp` Instruction:** We bypass standard clock functions and use the `rdtscp` hardware instruction directly in C++ for nanosecond-precision timestamps. This guarantees the measurement tool introduces minimal jitter to the timeline.
- **`SO_BUSY_POLL` & `CAP_NET_ADMIN`:** By busy-polling the network socket, we eliminate kernel interrupt latency on the hot path. This requires running the bot fleet as a privileged DaemonSet (`NET_ADMIN`), but it's essential for achieving sub-microsecond precision.
- **`--network none` in Builder:** When compiling contestant code, we entirely disable networking to prevent build-time telemetry exfiltration or downloading unverified dynamic dependencies. It enforces a truly hermetic build.
- **Single-Producer Single-Consumer (SPSC) Ring Buffers:** Used for offloading telemetry from the hot thread. We use lock-free, cache-line-aligned rings (with relaxed/acquire/release memory ordering) because mutexes would introduce thread contention that would artificially inflate the contestant's latency score.

---

## 13. Production Roadmap
1. Telemetry gateway rewritten in Rust/Go reading VictoriaMetrics.
2. HW NIC timestamp `cmsg` parsing on a PTP-synced fleet for true 4-way latency decomposition.
3. AF_XDP / SIMD ingest once per-node receive crosses ~1 M msg/s.
4. Multi-symbol sharded books with one writer thread per symbol shard.

> **One sentence to leave you with:** Anyone can draw a leaderboard. We built the one that refuses to show you a number it can't stand behind.

## Considered Alternatives (and why not)

- **gRPC** — rejected on the measured wire (HTTP/2 + protobuf would add tens of µs to the hot path); fine for the control plane. The intake API is plain HTTP so it stays curl-able. The bot↔engine contract is fixed-layout binary, parsed by pointer-cast.
- **Kafka / Redpanda** — not on the telemetry path. HDR histograms are additively mergeable, so bots ship pre-aggregated blobs cold; a log bus adds a hop + serialization for durable replay we don't need at benchmark timescale.
- **VictoriaMetrics over TimescaleDB / InfluxDB** — high-cardinality histogram aggregation at a fraction of the RAM, where TimescaleDB / Prometheus struggle (documented scale-out target; demo path is CSV snapshots → gateway).
- **Redis** — used, not just discussed: the submission work-queue in `platform/intake-api/` (control plane, never the hot path).
