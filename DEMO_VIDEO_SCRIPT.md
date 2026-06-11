# Demo Video Script — IICPC HFT Benchmarking Platform
### Target runtime: 6:00 (within the 5–7 min window) · One narrator + screen capture

> **The single thread that ties the whole video together:**
> *"Most latency benchmarks lie. We built the one that refuses to."*
> Open with it, prove it in the middle, close on it.

**Before you hit record — read this:**
- The video is graded on **engineering reasoning**, not visual polish. For every
  feature shown, say *why it exists and what breaks without it*. Depth wins.
- Keep cuts tight. No dead air watching things compile — pre-build, then cut to
  the result.
- Honesty reads as competence. The "reading the numbers honestly" beat (§5) is a
  *strength*, not an apology. Lean into it.

---

## Pre-record setup (do this off-camera)

```bash
# 1) Pre-build so the video never waits on a compile
cd bot-engine && mkdir -p build && cd build && cmake .. && make -j && cd ..

# 2) Terminal A — live telemetry feed
cd tools && npm install && node telemetry_server.js --port 8080
#    → ws://localhost:8080/leaderboard/deltas

# 3) Terminal B — serve the leaderboard
cd frontend && python3 -m http.server 8088     # → http://localhost:8088
```

Tabs open and ready: **(1)** the live leaderboard, **(2)** `landing.html` showcase,
**(3)** a clean terminal in `bot-engine/`, **(4)** `ARCHITECTURE.md` /
`DESIGN_DOCUMENT.md` for the close.

Pre-stage the CO-proof and demo commands in shell history (press ↑, don't type).

---

## SHOT LIST & SCRIPT

### 0:00 – 0:30 — The hook (cold open, no logo)

**On screen:** the `landing.html` showcase, slow scroll into the glowing core →
cut to the live leaderboard with rows updating.

**Narration:**
> "Most latency benchmarks lie. Not on purpose — they have a structural bug
> called *Coordinated Omission* that hides the exact stalls that matter, and they
> report a beautiful p99 that never happened.
> We were asked to build a platform that hosts contestant trading engines,
> stress-tests them with a distributed bot fleet, and ranks them on latency,
> throughput, and correctness. So we built it around one promise: **measure the
> truth, then only display it.** Here's how."

---

### 0:30 – 1:30 — The architecture (the 30,000-ft view)

**On screen:** the ASCII diagram in `ARCHITECTURE.md` (or a clean slide of it).
Point as you talk.

**Narration:**
> "Three components, built in parallel by three people, bound by **one frozen
> binary wire contract**.
> On the left, a **Sandbox**: it takes an untrusted submission and runs it inside
> a **Firecracker microVM** — not a container. That's deliberate: a container
> shares the host scheduler, so a noisy neighbour would show up as the
> submission's tail latency. A microVM gives a real kernel and scheduling
> boundary, so we measure *their* engine, not our noise.
> In the middle, the **Bot Fleet and Telemetry** — an open-loop, CPU-pinned C++
> load generator that measures latency honestly, plus a gold-standard reference
> engine and a byte-exact correctness validator.
> On the right, a **display-only leaderboard**. It never computes a percentile —
> it physically *can't* introduce a measurement lie, because it only renders what
> the scoring service already proved.
> The contract is frozen and versioned: one field change breaks three codebases,
> so we froze it on day one. That's what let us build in parallel without
> integration hell."

---

### 1:30 – 3:00 — The centrepiece: Coordinated Omission, proven live

**On screen:** Terminal in `bot-engine/`. Run the pre-staged command; let the
two-histogram table print.

```bash
./build/null_responder --stall-mode --stall-every 20000 --stall-ms 5 &
./build/bot --bots 1 --interval-us 100 --duration-sec 60 --no-gate
```

**Narration (while it runs ~10s, then cut to the result table):**
> "Here's the proof. I'm pointing a single bot at a server I've rigged to stall
> for exactly **5 milliseconds every 20,000 orders** — a planted, known defect.
> A naive benchmark *cannot see this stall*. When the engine freezes, the bot
> freezes too — it can't send orders during the stall — so the samples that
> should have landed in those 5 milliseconds are simply never taken. The naive
> histogram shows a clean p99 and calls the engine fast.
> Our bot tags every order with its **intended** send time, not the time it
> actually got to send. So a stall shows up as **latency**, not as missing data —
> and a second histogram backfills the samples the stall would have omitted."

**On screen:** highlight the two p99 columns.

> "Look at the two numbers side by side. Naive p99: **98 microseconds** — 'great
> engine.' Coordinated-Omission-corrected p99: **3.4 milliseconds** — a **35-times
> gap.** The 3.4 ms is the truth — that's exactly the stall I injected. *This* is
> the number every contestant on our platform is ranked on. The honest one."

---

### 3:00 – 3:50 — Correctness: the byte-exact validator

**On screen:** run the demo; let the `DIVERGE @ byte 244` output print.

```bash
./demo/run_demo.sh
```

**Narration:**
> "Speed is only half the score — an engine can be fast by *cheating*. So we ship
> a gold-standard reference matching engine and diff every contestant's output
> against it, **byte for byte**.
> This demo plants a real-world exchange bug: LIFO matching — the *newest* order
> fills first instead of the oldest — a price-time-priority violation that real
> exchanges have actually shipped. It produces an output the exact same *length*
> as the correct one, so a threshold-based checker would wave it through.
> Our validator pinpoints it: **divergence at byte 244**, message #7 — the first
> aggressive trade — wrong order matched. No heuristics, no thresholds. And on the
> leaderboard a correctness failure **hard-caps the score** — speed never buys
> back incorrectness."

---

### 3:50 – 5:00 — The leaderboard (live, and why it's trustworthy)

**On screen:** the live leaderboard. Click the protocol bar FIX → WS → REST.
Hover the percentile ladder. Point at a greyed/excluded row and a FAIL score.

**Narration:**
> "This is the live board — real-time deltas over WebSocket, not a screenshot.
> First: **separate boards per protocol.** We never mix FIX, WebSocket, and REST —
> TLS and JSON framing alone add tens of microseconds, so ranking them together is
> a category error. The contract forbids it; the UI enforces it.
> Second: the **full percentile ladder** — p50 through p99.99 to max, in
> nanoseconds. No average latency *anywhere* — an average hides the exact stalls
> we just proved matter.
> Third — this greyed row: it was **excluded before ranking.** Its measuring bot
> failed a self-test integrity gate, meaning the *instrument* was noisier than what
> it was measuring. We'd rather drop a number than show a dishonest one — that's a
> filter, not a penalty.
> And the fleet p99 here is read from a **merged HDR histogram**, never an average
> of per-bot p99s — because averaging percentiles is mathematically meaningless."

**On screen (optional, 5s):** kill the gateway, show the board fall back to mock
without going blank.

> "And it's mock-first: if the live feed drops, the board falls back instantly —
> a demo never goes blank."

---

### 5:00 – 5:40 — Proof it's deploy-ready & verified

**On screen:** split or quick cuts — `RESULTS.md` table, `infra/k8s/platform.yaml`,
`infra/terraform/benchmark_pool.tf`.

**Narration:**
> "None of this is hand-wavy. The whole suite runs under **AddressSanitizer and
> UBSan with zero findings** — the hot path allocates nothing after startup, and
> now that's proven, not claimed. We've driven **32 bots on 4 cores** — 8× over-
> subscription — with every single order accounted for, zero collisions. The
> matching engine passes **19 of 19** unit tests, and a live TCP run matched an
> offline replay **byte-for-byte across 12.9 million bytes.**
> For deployment there are two paths: a one-command Docker Compose for the demo,
> and the real one — a bare-metal Kubernetes **DaemonSet** with Guaranteed-QoS
> pinned cores, on Terraform-provisioned nodes with `isolcpus` and `nohz_full`, so
> the CPU-pinning and busy-poll features actually engage. And we're explicit about
> which path produces real numbers and which is for the demo."

---

### 5:40 – 6:00 — The close

**On screen:** back to the leaderboard / the showcase core, slow push-in.

**Narration:**
> "We deliberately built three components *well* instead of four halfway. We
> skipped AVX-512, AF_XDP, and Aeron — and we can tell you the exact throughput at
> which each would start to matter, which is why they're documented as decisions,
> not gaps.
> Anyone can draw a leaderboard. We built the one that **refuses to show you a
> number it can't stand behind.** Thanks for watching."

---

## Contingencies (if something breaks mid-record)

| Problem | Fallback |
|---|---|
| Live feed down | Leaderboard auto-falls-back to mock — visually identical; keep going |
| C++ won't build on the machine | Show the verified CO-proof table in `RESULTS.md` instead of a live run |
| On macOS | Say it up front: "CPU pinning is a no-op here, so you'll see scheduler jitter — on the bare-metal K8s path that disappears." Honesty = competence |
| Running long | Cut the optional mock-fallback beat (4:55) and trim the architecture intro to 45 s |

## Timing cheat-sheet

| Segment | Budget | Cumulative |
|---|---|---|
| Hook | 0:30 | 0:30 |
| Architecture | 1:00 | 1:30 |
| CO proof (centrepiece) | 1:30 | 3:00 |
| Correctness validator | 0:50 | 3:50 |
| Live leaderboard | 1:10 | 5:00 |
| Deploy-ready + verified | 0:40 | 5:40 |
| Close | 0:20 | 6:00 |

**Spend the most time on the CO proof.** It is the one thing no other team will
have, and it is the entire reason this platform deserves to win.
