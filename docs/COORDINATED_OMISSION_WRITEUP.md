# Coordinated Omission in an Open-Loop HFT Benchmark: a Measurement Note

Mohit Prajapati — IICPC 2026 "Bad Apples" benchmarking platform
Repo: `bot-engine/` in this repository. All numbers below are read from
[`verified_runs/canonical.json`](../verified_runs/canonical.json), which is the single
source of truth this repo's docs and dashboards draw from — nothing here is
hand-typed.

## 1. What this is

A load generator ("bot") drives a matching engine over a custom binary wire
protocol and measures order-acknowledgment latency. The generator records two
numbers per order: a naive round-trip time, and a coordinated-omission (CO)
corrected round-trip time. Under a controlled 5&nbsp;ms server-side stall, the
naive measurement understates p99 latency by **19.7×** on a shared host and
**76.3×** on an isolated one. This note describes the mechanism precisely,
gives exact reproduction steps, and states what the result does and does not
prove.

The correction follows Gil Tene's coordinated-omission analysis directly —
*"How NOT to Measure Latency"*, QCon London 2013 (longer version at Strange
Loop 2015) — and uses the reference implementation from HdrHistogram
(`hdr_record_corrected_value`, the C port of `recordValueWithExpectedInterval`)
rather than a reimplementation of the correction math.

## 2. Setup

**Topology.** One bot thread opens a TCP connection to a matching engine and
sends `NewOrder` messages at a fixed cadence; the engine replies with
`OrderAck`. For the CO-proof run specifically, the matching engine is replaced
by a test double, `null_responder`, that does nothing except ack every order —
except every *N*th order, where it sleeps for a fixed duration before acking.
This isolates the measurement question from engine internals: the only thing
that can go wrong is in the bot's timing code, not in matching logic.

**Closed-loop vs. open-loop, as used here.** A closed-loop generator waits for
the response to request *i* before issuing request *i+1*; when the server
stalls, the generator stalls with it, so the requests that *would* have been
sent during the stall are never sent, and never measured. An open-loop
generator issues requests on a fixed wall-clock schedule regardless of whether
prior responses have arrived. The bot (`bot-engine/src/bot.cpp`) is open-loop
by construction: it maintains a virtual send-time grid (`next_send_time`,
incremented by a fixed `interval_ns` every iteration) that is never reset or
delayed by ack arrival.

**Hardware and pinning.** Two conditions are committed:
- *Shared host*: Arch Linux, 16 logical cores, no `isolcpus`, described as a
  "shared dev host" — other processes compete for the same cores.
- *Isolated host*: Intel Core i7-13620H, booted with
  `isolcpus=0,1 nohz_full=0,1 rcu_nocbs=0,1`, bot pinned to core 1 via
  `sched_setaffinity`, `SO_BUSY_POLL` (50µs) and `TCP_NODELAY` set on the
  socket. This is the same physical machine used for the separately-committed
  bare-metal ladder (`verified_runs/aftab/baremetal_latency.txt`, p99 = 7.7µs
  clean-run, no stall).

I could not independently re-verify from the committed logs alone that the
*shared*-host run was taken on different physical hardware from the isolated
one — the shared-host log records CPU count and OS but not a CPU model string.
That is a real gap in the evidence and is called out again in §7.

**Timing.** `hft::rdtscp_ns()` (`bot-engine/src/tsc_util.h`) reads the invariant
TSC via `rdtscp` and scales it to nanoseconds using a ratio calibrated once at
startup against `std::chrono::steady_clock` over a 100&nbsp;ms window. The
header documents its own limitation: this is only valid if every timestamped
thread runs on the same physical socket, because independent sockets have
independently drifting oscillators. Multi-socket production use would need
NIC hardware timestamping (PTP) instead — noted as a roadmap item, not
implemented here.

**Workload (CO-proof run).** 600,000 orders, one bot, 100µs nominal
send interval, single TCP connection. `null_responder` runs in
`--stall-mode --stall-every 20000 --stall-ms 5`: every 20,000th order it
receives, it sleeps 5&nbsp;ms before sending the ack, deterministically and on
the server side only — the client's send path is never blocked by this sleep.

## 3. The bug: how coordinated omission produces the gap

For every order, the bot's pending-order table (`PendingSlot`, keyed by
`seq & PENDING_MASK`) stores two timestamps taken with `rdtscp_ns()`:

- `intended_ts` — the virtual grid time this order *should* have been sent at.
- `actual_send_ts` — the real time the `send()` syscall returned.

On ack, it computes both:
```
naive_latency = ack_time - actual_send_ts   // conventional RTT
co_latency    = ack_time - intended_ts      // trader-felt latency
```
(`bot-engine/src/bot.cpp:688-689`)

During a 5&nbsp;ms stall, TCP happily buffers the handful of tiny (44-byte)
`NewOrder` frames the bot keeps emitting on schedule — the client is never
gated by the stall, so `actual_send_ts ≈ intended_ts` for most orders and
`naive_latency` stays near the baseline RTT. What changes is `ack_time`: every
order whose processing falls inside the stall window gets its ack delayed
until the responder wakes up, so `co_latency` for those orders correctly
balloons toward 5&nbsp;ms. This is the textbook coordinated-omission failure
mode, reproduced without a closed loop: a naive benchmark that only measures
`ack - actual_send` cannot see the stall at all, because it never asks "when
was this request *supposed* to happen."

**A subtlety worth stating precisely, because it's not obvious and I checked
it empirically rather than assert it.** `bot.cpp` does not stop at
`co_latency`; it also runs that value through
`hdr_record_corrected_value(hist, co_latency, interval_ns)`
(`bot-engine/src/bot.cpp:570-571`), which is HdrHistogram's own interval-based
backfill: for any recorded value larger than `interval_ns`, it additionally
inserts a staircase of synthetic samples at `value - interval_ns`,
`value - 2·interval_ns`, ... down to `interval_ns`. Since `intended_ts`
already gives an honest per-order latency with no missing samples (the bot's
pending-slot pool is sized 2^20 — vastly larger than the ~50 orders that back
up during one 5&nbsp;ms stall at 100µs cadence — so no virtual slot is ever
silently skipped for lack of a free slot), it is fair to ask whether this
extra backfill double-counts.

I built a standalone reproduction (not the repo build, which is x86-only and
won't compile on my arm64 machine — see §6) that replays exactly this
accounting against the real `hdr_histogram_c` library, once recording
`co_latency` directly and once through `hdr_record_corrected_value`. Direct
recording puts the stall-affected fraction (≈0.25% of orders, matching 30
stalls × ~50 orders / 600,000) at **p99.9**, not p99 — it's genuinely a rare
event at the per-order level. Running the same values through
`hdr_record_corrected_value` pads the tail with enough synthetic mass that the
stall becomes visible at **p99**. So the backfill is not redundant: it
re-expresses "fraction of *orders* affected" as "fraction of *operating time*
affected" — the metric a continuously-present trader actually cares about,
since at any random instant during the stall window a hypothetical order
would have been late, even though only a minority of the 600,000 discrete
orders happen to land there. This is the same choice HdrHistogram's own docs
recommend making generally, independent of open- vs. closed-loop generation.
I did not verify this end-to-end on the real compiled bot; the synthetic
reproduction matches the shape and order of magnitude of the committed
phantom-sample counts (my model: 35,574 backfilled samples against a
simplified stall/RTT model; committed runs: 53,584 and 49,566) but is not a
byte-for-byte replay of the network path.

## 4. The fix

Two things, stacked, both necessary:

1. **Never let request generation depend on prior responses.** The bot's send
   loop increments its virtual clock unconditionally; a stalled server delays
   acks, not future sends. This is Tene's prescribed cure for coordinated
   omission and is what makes `intended_ts` meaningful in the first place.
2. **Record against the real HdrHistogram correction, not just the raw
   corrected timestamp**, for the reason given in §3 — it reports the
   operationally relevant quantity, not just the discrete-order quantity.

Both naive and CO histograms are computed from the same acked-order stream in
the same process, so there is no separate "before/after" run to introduce
run-to-run variance into the comparison — the 19.7×/76.3× ratios come from one
run each, naive and corrected computed side by side per order.

## 5. Why the ratio moves with host isolation (19.7× → 76.3×)

The workload is identical in both conditions — same 600,000 orders, same
100µs interval, same 5&nbsp;ms/20,000-order injected stall. The
CO-corrected p99 is essentially the injected stall and stays close to
3.1–3.4&nbsp;ms in both:

| | naive p99 | CO-corrected p99 | ratio |
|---|---|---|---|
| shared host | 173,439 ns | 3,414,015 ns | 19.7× |
| isolated (`isolcpus`) | 41,119 ns | 3,135,487 ns | 76.3× |

What moves is the **naive baseline**, not the injected fault. On the shared
host, ordinary scheduler preemption, timer-tick interrupts, and competing
processes push the tail of the *ordinary* (non-stalled) round trips out to
~173µs at p99 — the ambient jitter is large enough to blur into the stall
itself at the percentile level. On `isolcpus=0,1 nohz_full=0,1
rcu_nocbs=0,1` cores dedicated to nothing else, with the scheduler tick
disabled and no other runnable work, ordinary round trips stay tight (naive
p99 = 41µs — close to the same machine's clean-run p99 of 7.7µs with no stall
at all). Since the ratio is `corrected / naive` and the numerator is pinned by
the fixed 5&nbsp;ms fault, a cleaner host mechanically produces a *bigger*
ratio for the exact same fault — the isolation doesn't change what's being
hidden, it changes how good the naive number looks by comparison, which is
precisely why the ratio is host-dependent and not a fixed constant.

## 6. Reproducibility

```bash
git clone https://github.com/mohitt31/iicpc-hackathon-2026.git
cd iicpc-hackathon-2026/bot-engine
# Requires: hdr_histogram_c (brew install hdrhistogram on macOS,
# apt install libhdrhistogram-dev on Debian/Ubuntu), CMake, a C++20 compiler.
mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target bot null_responder

./null_responder --stall-mode --stall-every 20000 --stall-ms 5 &
./bot --bots 1 --interval-us 100 --duration-sec 60 --no-gate
```
This is exactly the command pair in `RESULTS.md` §4. Note:
`CMakeLists.txt` hardcodes `-march=x86-64-v2`, so this builds on x86_64 Linux
only; it does not build on Apple Silicon without editing that flag (this is
a real, unresolved limitation of the current build — see §7). The isolated
run additionally needs a kernel booted with `isolcpus=0,1 nohz_full=0,1
rcu_nocbs=0,1` and the bot pinned to one of those cores.

## 7. Limitations — what this does and doesn't prove

- **The stall is synthetic and deterministic**, injected by a test double on
  a fixed schedule. It is a clean way to isolate the measurement mechanism,
  but it is not a GC pause, a page fault, a NIC interrupt storm, or any other
  organic source of tail latency. The claim is "this measurement technique
  correctly surfaces a known, injected fault"; it is not yet "this is what a
  real matching engine's tail looks like in production."
- **The `hdr_record_corrected_value` stacking (§3) has not been verified on
  the actual compiled binary** — only in a standalone model against the real
  library. I'd want to build on Linux/x86 (or retarget the CMake flags for
  arm64) and confirm the committed 53,584 / 49,566 phantom-sample counts
  reproduce a comparable "which percentile does the stall first appear at"
  shift before I'd call this fully closed.
- **The two hardware conditions are not a controlled A/B on identical
  hardware** from the committed evidence alone — the shared-host run's exact
  CPU model isn't logged. The mechanism explanation in §5 is sound
  independent of this (it only requires that isolation reduces ambient
  jitter, which is uncontroversial), but I can't cite it as "same box,
  isolation toggled" with the same confidence as the rest of the run.
- **Single bot, single connection.** This isolates the CO mechanism cleanly
  but says nothing about behavior under concurrent load; the platform's
  2,989-connection scale run is a separate, throughput/accounting result
  (`verified_runs/canonical.json` → `scale_thousands`) and does not re-run the
  CO comparison at that concurrency.
- **The specific numbers "~2ms naive / ~100ms corrected" I described verbally
  earlier are wrong** and are retracted here in favor of the traced values
  above (173µs→3.41ms shared; 41µs→3.14ms isolated). The 19.7×–76× ratio was
  correct; the absolute latencies I stated were not.
- **rdtscp/TSC timing is single-socket-valid only** (documented in
  `tsc_util.h`); this run pins to one core/socket so it's within that bound,
  but it is not the wire-to-wire, NIC-timestamped measurement a production
  system would need.

## References

- G. Tene. *How NOT to Measure Latency.* QCon London, 2013 (extended version:
  Strange Loop, 2015).
- P. Cai and M. Karsten. *Kernel vs. User-Level Networking: Don't Throw Out
  the Stack with the Interrupts.* Proc. ACM Meas. Anal. Comput. Syst.
  (SIGMETRICS 2024). https://doi.org/10.1145/3626780
- HdrHistogram C port (`hdr_histogram_c`), `hdr_record_corrected_value` /
  `recordValueWithExpectedInterval` semantics.
