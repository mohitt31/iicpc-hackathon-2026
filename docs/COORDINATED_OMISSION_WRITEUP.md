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
naive measurement understates p99 latency by **19.7×–93.8×** depending on
host (§6). A companion tool built for this note goes further and reproduces
the textbook failure mode directly: a genuine closed-loop generator against
the identical fault reports p99, p99.9, and even **p99.99 as completely
normal** — the fault is only visible at p99.999 (§4). This note describes the
mechanism precisely, gives exact reproduction steps, and states what the
result does and does not prove.

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
That is a real gap in the evidence and is called out again in §8.

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

I checked this two ways rather than assert it. First, a standalone model
against the real `hdr_histogram_c` library (not the repo build itself — at
the time, `CMakeLists.txt` hardcoded an x86-only compiler flag and wouldn't
even configure on my arm64 machine; fixed since, see §7): recording
`co_latency` directly put the stall-affected fraction at p99.9, not p99;
running the same values through `hdr_record_corrected_value` pushed it to
p99. Second — since a model isn't the same as the real thing — I added
`bot-engine/src/co_correction_probe.cpp`,
a diagnostic tool (not part of the judged demo path) that drives the actual
wire protocol against the actual `null_responder`, computing the same
`ack - intended_ts` per order and recording it into two real histograms in
parallel: one via `hdr_record_value` only, one via
`hdr_record_corrected_value`. It ran on GitHub Actions' `ubuntu-latest`
(x86, ephemeral), workflow `co-correction-probe.yml`, run
[30469631810](https://github.com/mohitt31/iicpc-hackathon-2026/actions/runs/30469631810),
evidence committed at `verified_runs/ci/co_correction_probe_x86.txt`:

| | p99 | p99.9 |
|---|---|---|
| plain (`hdr_record_value` only) | 35,935 ns | 3,274,751 ns |
| corrected (`hdr_record_corrected_value`) | 3,198,975 ns | 4,558,847 ns |

This confirms the model exactly: without the backfill, the stall shows up at
p99.9 (3.27ms), matching almost exactly where the *corrected* column's p99
lands (3.20ms) — the backfill's effect is precisely to move the visible onset
of the stall from p99.9 to p99, not to change its magnitude. Phantom count on
this run was 41,626 (600,000→641,626), against 41,676 independently reported
by the actual `bot` binary in the companion sanity-check run in the same CI
job (`verified_runs/ci/co_proof_ci_x86.txt`) — two independent
implementations of the same accounting, agreeing to within 0.1%. So the
backfill is not redundant: it re-expresses "fraction of *orders* affected"
(≈0.25%, genuinely rare per-order) as "fraction of *operating time* affected"
— the metric a continuously-present trader actually cares about, since at any
random instant during the stall window a hypothetical order would have been
late, even though only a minority of the 600,000 discrete orders happen to
land there. This is the same choice HdrHistogram's own docs recommend making
generally, independent of open- vs. closed-loop generation.

That CI run also handed back a third, independent host data point for §6: on
GitHub's shared runner, the same workload gave naive p99 = 34,623 ns →
corrected p99 = 3,246,079 ns, a **93.8×** ratio — above even the isolated
i7-13620H's 76.3×, consistent with a noisy, virtualized shared runner having
its own unpredictable baseline jitter. Three environments now show the same
qualitative pattern (19.7×, 76.3×, 93.8×) with three different naive
baselines and the same ~3.1–3.4ms corrected tail, which is the strongest
evidence in this note that the ratio tracks the *naive baseline*, not the
fault.

## 4. Direct evidence: what a naive closed-loop generator would have shown

Everything above measures an *open-loop* generator two ways (naive vs.
CO-corrected). It's worth showing the actual failure mode Tene's talk warns
about, not just describing it: what happens if the generator is closed-loop —
textbook wrong — against the identical fault?

`bot-engine/src/closed_loop_probe.cpp` sends one order, blocks until its ack
arrives, only then sends the next. One request in flight, ever. Run against
the same `null_responder --stall-mode --stall-every 20000 --stall-ms 5` for
60 seconds (Apple M4, `verified_runs/mac_arm64/closed_loop_mac_arm64.txt`;
reproduced on GitHub Actions x86, `verified_runs/ci/closed_loop_probe_x86.txt`):

| percentile | latency |
|---|---|
| p50 | 12,399 ns |
| p90 | 14,543 ns |
| p99 | 18,895 ns |
| p99.9 | 48,319 ns |
| **p99.99** | **112,127 ns** |
| **p99.999** | **6,311,935 ns** |

4,710,837 round trips completed in the 60 seconds. The fault fired roughly
every 20,000 of them (≈235 times), so it affects roughly 1 in 20,000 samples
— **below the p99.99 threshold**. The result: p99, p99.9, even p99.99 all
report numbers under 112µs, giving zero indication that a 5&nbsp;ms stall was
injected 235 times during the run. It only appears at p99.999. A dashboard
built on this generator with a "p99.9 SLO" would report the system as healthy
throughout.

Cross-checked on GitHub Actions x86
(`verified_runs/ci/closed_loop_probe_x86.txt`, run
[30471578420](https://github.com/mohitt31/iicpc-hackathon-2026/actions/runs/30471578420),
1,629,698 round trips): p99 = 49,983 ns, p99.9 = 114,431 ns,
p99.99 = 281,855 ns, p99.999 = 5,115,903 ns — same qualitative result on a
second, independent architecture: blind through p99.99, visible only at
p99.999.

This is the reason the bot uses an open-loop generator at all — not a
stylistic choice, a measurement necessity. At the open-loop's 100µs cadence,
the same 5&nbsp;ms stall affects ~50 consecutive virtual slots per
occurrence instead of 1 (because the generator keeps emitting instead of
blocking), which is what makes the fault visible three orders of magnitude
earlier in the percentile curve (p99.9, then p99 once
`hdr_record_corrected_value` is applied per §3) instead of needing p99.999.

## 5. The fix

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

## 6. Why the ratio moves with host isolation (19.7× → 76.3× → 93.8×)

The workload is identical in all three conditions — same 600,000 orders, same
100µs interval, same 5&nbsp;ms/20,000-order injected stall. The
CO-corrected p99 is essentially the injected stall and stays close to
3.1–3.4&nbsp;ms across all of them:

| | naive p99 | CO-corrected p99 | ratio |
|---|---|---|---|
| shared host (Arch Linux) | 173,439 ns | 3,414,015 ns | 19.7× |
| isolated (`isolcpus`, i7-13620H) | 41,119 ns | 3,135,487 ns | 76.3× |
| GitHub Actions shared runner (§3) | 34,623 ns | 3,246,079 ns | 93.8× |

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

**Aside — run-to-run variance, not just host variance.** After fixing the
build to run on arm64 (§7), two back-to-back CO-proof runs on the same Apple
M4 laptop (busy running this analysis at the time — not a clean host, see
`verified_runs/mac_arm64/README.md`) gave naive p99 of 40,191 ns and
61,247 ns and ratios of **108.9×** and **174.7×** respectively, on
*identical* hardware and fault, seconds apart. Separately, re-running the
exact same GitHub Actions CI job a second time
(`verified_runs/ci/co_proof_ci_x86_run2.txt`, run
[30471578420](https://github.com/mohitt31/iicpc-hackathon-2026/actions/runs/30471578420))
gave **72.1×** where the first run gave 93.8× — same shared-runner
infrastructure, same workload, different result. I'm not treating any of this as additional
rigor-matched hosts — they're single-shot trials on two already-counted
environments — but it's worth stating plainly: this note reports single-trial
point estimates throughout, not means over repeated runs with confidence
intervals. Four extra data points (108.9×, 174.7×, 93.8×, 72.1×) on top of
the three in the table above all say the same thing louder: a properly
rigorous version of this benchmark would repeat each condition N times and
report a distribution of ratios, not one number.

## 7. Reproducibility

```bash
git clone https://github.com/mohitt31/iicpc-hackathon-2026.git
cd iicpc-hackathon-2026/bot-engine
# Requires: hdr_histogram_c (brew install hdrhistogram on macOS,
# apt install libhdrhistogram-dev on Debian/Ubuntu), CMake, a C++20 compiler.
mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target bot null_responder co_correction_probe closed_loop_probe

./null_responder --stall-mode --stall-every 20000 --stall-ms 5 &
./bot --bots 1 --interval-us 100 --duration-sec 60 --no-gate
```
This is exactly the command pair in `RESULTS.md` §4. `co_correction_probe`
(§3) and `closed_loop_probe` (§4) run the same way, against the same
`null_responder`:
```bash
./null_responder --stall-mode --stall-every 20000 --stall-ms 5 --port 9501 &
./co_correction_probe --orders 600000 --interval-us 100 --port 9501

./null_responder --stall-mode --stall-every 20000 --stall-ms 5 --port 9502 &
./closed_loop_probe --duration-sec 60 --port 9502
```
`CMakeLists.txt` used to hardcode `-march=x86-64-v2` unconditionally, which
meant the whole tree failed to even configure a compile on Apple Silicon —
this has been fixed (the flag is now conditional on
`CMAKE_SYSTEM_PROCESSOR`; the x86_64 build is byte-for-byte unaffected), so
all of the above now also builds and runs directly on arm64. The isolated
run additionally needs a kernel booted with `isolcpus=0,1 nohz_full=0,1
rcu_nocbs=0,1` and the bot pinned to one of those cores. The CI job is
`.github/workflows/co-correction-probe.yml`, triggerable via
`gh workflow run co-correction-probe.yml` or on push to any of the probe
sources.

## 8. Limitations — what this does and doesn't prove

- **The stall is synthetic and deterministic**, injected by a test double on
  a fixed schedule. It is a clean way to isolate the measurement mechanism,
  but it is not a GC pause, a page fault, a NIC interrupt storm, or any other
  organic source of tail latency. The claim is "this measurement technique
  correctly surfaces a known, injected fault"; it is not yet "this is what a
  real matching engine's tail looks like in production."
- **The two hardware conditions in the original run are not a controlled A/B
  on identical hardware** from the committed evidence alone — the shared-host
  run's exact CPU model isn't logged. The mechanism explanation in §6 is sound
  independent of this (it only requires that isolation reduces ambient
  jitter, which is uncontroversial), but I can't cite it as "same box,
  isolation toggled" with the same confidence as the rest of the run.
- **Single-trial point estimates, no confidence intervals** — see the aside
  in §6. This is the most generically-applicable limitation in this note and
  the one I'd fix first given more time.
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
