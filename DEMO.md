# Demo Script — IICPC HFT Benchmarking Platform

A tight walkthrough for the judging round. Total runtime ~4 minutes; the live
**2-minute version** is marked with ▶. Each beat ties a visible thing on screen
to the one idea that wins: **we measure latency honestly, then only display it.**

---

## 0. Before judges arrive (setup, ~2 min)

```bash
# Terminal A — start the live telemetry feed (no teammates / backend needed)
cd tools
npm install            # one-time, pulls 'ws'
node telemetry_server.js --port 8080
#   → ws://localhost:8080/leaderboard/deltas

# Terminal B — serve the leaderboard
cd frontend
python3 -m http.server 8088
#   → http://localhost:8088
```

To show the **live** path, open `frontend/index.html` and set near the top:
```js
const USE_LIVE = true;
const LIVE_WS_URL = 'ws://localhost:8080/leaderboard/deltas';
```
(Leave `USE_LIVE=false` to demo the in-page mock — visually identical, zero
setup. Either is a legitimate demo; live is more impressive if the feed is up.)

Have open in tabs: the **leaderboard**, the **bot-engine demo** terminal, and
`ARCHITECTURE.md`.

---

## 1. The hook — 20 seconds ▶

> "Most latency benchmarks lie. Ours is built around the one bug that makes them
> lie — Coordinated Omission — and everything you'll see is measured the honest
> way."

Open the leaderboard. Let it sit; rows update live.

---

## 2. The leaderboard — 60 seconds ▶

Point at the **giant hero name** at the top:
> "That's whoever's currently winning the BINARY (SBE) board. It changes live as the
> benchmark runs — this is real-time, not a screenshot."

Click through the **orange protocol bar** — BINARY → WebSocket → REST:
> "Separate boards per protocol. We never mix them — comparing BINARY to REST is
> meaningless, TLS and JSON framing alone add tens of microseconds. The contract
> forbids it, so the UI enforces it."

Point at the **percentile ladder** (p50 → p99.99 → max):
> "Full tail, in nanoseconds. No average latency anywhere — an average hides
> exactly the stalls that matter."

Point at a **greyed-out / excluded row**:
> "This submission was *excluded before ranking* — its measuring bot failed a
> self-test integrity gate. We'd rather drop a number than show a dishonest one.
> That's a filter, not a penalty."

Point at the **PTP badge** and a **FAIL** score:
> "Clock-sync quality is shown on every board. And correctness is a hard cap —
> a byte-exact diff failure caps the score. Speed never buys back incorrectness."

---

## 3. The honest-measurement proof — 60 seconds

Switch to the bot-engine terminal:

```bash
cd bot-engine && mkdir -p build && cd build && cmake .. && make -j && cd ..

# Coordinated Omission proof — a deterministic 5ms stall every 20k orders
./build/null_responder --stall-mode --stall-every 20000 --stall-ms 5 &
./build/bot --bots 1 --interval-us 100 --duration-sec 30 --no-gate
kill %1
```

> "Same run, two histograms side by side. Naive p99 reads ~50 microseconds.
> CO-corrected p99 reads ~4 milliseconds — an 80× gap. The naive number is the
> lie every public benchmark tells. We backfill the samples the stall omits."

---

## 4. The correctness validator — 40 seconds

```bash
./demo/run_demo.sh
```

> "We ship a gold-standard reference engine. Every contestant's output is
> diffed byte-for-byte against it. This demo plants a real-world bug —
> price-time priority violation, newest order matching first — and the validator
> pinpoints the exact divergent byte on the first aggressive trade. No
> heuristics, no thresholds."

---

## 5. The architecture close — 30 seconds ▶

Show the diagram in `ARCHITECTURE.md`:

> "Three components, one frozen wire contract, built in parallel: a Firecracker
> sandbox that isolates submissions, a CO-corrected bot fleet that measures
> them, and a display-only leaderboard that can't lie because it never computes
> a percentile — it only renders what the scoring service already proved.
> Mock-first, with a zero-change swap to the live feed you're watching right
> now."

---

## If something breaks (graceful fallbacks)

- **Live feed down?** Set `USE_LIVE=false` — the leaderboard runs its built-in
  mock, visually identical. The demo does not depend on the gateway being up.
- **C++ won't build on the demo machine?** The CO proof numbers are in the root
  `README.md` (verified table); show those instead of a live run.
- **macOS?** Say so up front: "CPU pinning is a no-op here, so you'll see
  scheduler jitter — on the bare-metal K8s path that disappears." Honesty reads
  as competence.

---

## The one sentence to leave them with

> "Anyone can draw a leaderboard. We built the one that refuses to show you a
> number it can't stand behind."
