# Design-Doc Info Draft — for Manish to align to the 19-section judge format

> **What this file is.** Mohit's 30% deliverable: a single, exhaustive, *source-traced*
> brain-dump of every win, every number, the architecture, and the differentiators —
> gathered from `verified_runs/canonical.json`, `DESIGN_DOCUMENT.md`, and `RESULTS.md`.
> **Manish finalizes** by mapping these blocks onto the judge's 19-section format.
>
> **Iron rule (the brand):** every headline number below carries its `canonical.json`
> key-path or its `verified_runs/` evidence file. *Never hand-type a number into the
> final doc — copy it from the cited source.* One fabricated number lets an AI judge
> discredit all of them. The depth is the moat; the honesty is the brand.
>
> **Generated from:** `canonical.json` (`generated: 2026-06-13`, `schema_version 1.0`,
> `status: evidence-complete`). If canonical changes, regenerate this file.

---

## 0. The one-line thesis (lead with this)

> The hard part of a latency benchmark is not drawing the chart — it is **not lying in
> the chart**. The whole platform is built around one idea: **measure the truth, then
> only display it.**

Depth-over-breadth: three *correct* components instead of four half-built ones, with the
one bug that invalidates almost every public latency benchmark — **Coordinated Omission**
— made the spine of the system.

---

## 1. THE NUMBERS — every headline, with its canonical source

> Each row cites the `canonical.json` key-path and the committed evidence file. This is
> the table an AI judge will diff against the logs. Copy values from here verbatim.

### 1.1 The four protocol benchmarks (the cross-protocol story)

| Transport | naive p99 | Sent==Acked | Source |
|---|---|---|---|
| **Binary (SBE/TCP)** | **7.7 µs** (isolated bare-metal) | yes | `latency_design.baremetal_measured.p99_ns` = 7671 ns; `verified_runs/aftab/baremetal_latency.txt` |
| **WebSocket** | **223 µs** | 25,000 / 25,000 | `RESULTS.md` cross-protocol §; CI `wsrest-build.yml` |
| **FIX 4.4** | **890 µs** | 12,500 / 12,500 | `RESULTS.md` cross-protocol §; CI `wsrest-build.yml` |
| **REST (HTTP/1.1)** | **2,404 µs** | 12,500 / 12,500 | `RESULTS.md` cross-protocol §; CI `wsrest-build.yml` |

**Ordering = `binary ≪ WebSocket ≪ FIX ≪ REST`** — each step is the encoding tax,
exactly as theory predicts (binary = parse-by-pointer-cast; WS = RFC-6455 frame + masking;
FIX = SOH tag=value text + BodyLength + CheckSum; REST = HTTP headers + JSON encode/parse).
- **FIX 4.4 is the protocol the brief explicitly names** — and it lands exactly where
  theory says it should. Strong "we read the brief" signal.
- **Honest caveat (must keep):** WS/FIX/REST measured on a *shared CI runner*, not an
  isolated host, so absolute values carry scheduler jitter. The robust, reproducible
  claim is the **ordering** — which is precisely why the hot path is binary.

### 1.2 The bare-metal latency ladder (MEASURED — "design target" label RETIRED)

Source: `latency_design.baremetal_measured`; `verified_runs/aftab/baremetal_latency.txt`.
Host: **Intel i7-13620H**, `isolcpus=0,1 nohz_full=0,1 rcu_nocbs=0,1`, 300k/300k acked,
EAGAIN=0.

| Percentile | Value | canonical key |
|---|---|---|
| p50 | 5.9 µs | `p50_ns` = 5859 |
| p90 | 6.2 µs | `p90_ns` = 6155 |
| p99 | **7.7 µs** | `p99_ns` = 7671 |
| p99.9 | 11.3 µs | `p99_9_ns` = 11295 |
| p99.99 | 20.2 µs | `p99_99_ns` = 20175 |

- **It's MEASURED, not a target.** Every "design target — not yet measured" label is
  retired across surfaces (`open_items` TODO 4.4 round 2, item b).
- **Honest qualifier (keep it):** measured on an **isolated consumer desktop (i7-13620H),
  not server hardware**.
- **Built-in honesty check:** on this clean isolated run **naive == CO (ratio 1.0)** —
  `naive_equals_co: true` — because a quiet isolated machine has no coordinated-omission
  gap to hide. The methodology polices itself.
- Pushing median below ~5µs would need kernel-bypass (AF_XDP/Aeron) — **deliberately
  deferred** (see §4 differentiators + §9.1 of the design doc).

### 1.3 Coordinated Omission — the centerpiece (the moat)

Source: `coordinated_omission`; `RESULTS.md §4`; `bot-engine/verified_runs/ARCH_LINUX_RUN.md §3`;
`verified_runs/aftab/co_proof.txt`.

Run: 600,000 orders, 5 ms stall injected every 20,000 orders, 100µs interval, Arch Linux
16-core **no isolcpus** (shared dev host). 600,000/600,000 acked, 0 violations,
53,584 phantom samples backfilled (`coordinated_omission.run`).

| Percentile | Naive (ns) | CO-Corrected (ns) | Ratio | canonical key |
|---|---|---|---|---|
| p50 | 25,887 | 26,479 | 1.0× | `percentiles_ns.p50` |
| p90 | 80,895 | 141,823 | 1.8× | `percentiles_ns.p90` |
| p99 | 173,439 | 3,414,015 | **19.7×** | `percentiles_ns.p99` |
| p99.9 | 520,191 | 4,820,991 | 9.3× | `percentiles_ns.p99_9` |
| p99.99 | 5,189,631 | 5,251,071 | 1.0× | `percentiles_ns.p99_99` |

**The CO gap headline — say it exactly this way:**
> **"19.7× tabulated (shared host), up to 76× on isolated hardware."**

- `headline_gap.value` = "19.7x" — the conservative, fully-tabulated, line-by-line
  judge-verifiable figure.
- `headline_gap.isolcpus_run`: i7-13620H isolcpus measured naive p99 41,119ns →
  CO 3,135,487ns = **76.3×**, 600k/600k acked, 49,566 phantom backfilled
  (`verified_runs/aftab/co_proof.txt`).
- **RETIRED:** the earlier "~35×" prose (a ~98µs naive baseline with no committed table).
  Do **not** use 35× anywhere. Range is now **19.7×–76×** (`headline_gap.note`).
- Why the range is host-dependent (great soundbite): *the cleaner the host, the harder
  naive lies* — the baseline drops while the planted 5 ms stall stays fixed.

### 1.4 Byte-exact determinism (the strongest correctness claim)

Source: `correctness.byte_exact_replay`; `bot-engine/verified_runs/ARCH_LINUX_RUN.md §5`;
`RESULTS.md §3`; `verified_runs/aftab/live_replay.txt`.

- **Lead with the INVARIANT, not the number:** "Live TCP server output journal == offline
  replay, **byte-for-byte, IDENTICAL, exit 0**." (`byte_exact_replay.claim`)
- 200,000 orders (`orders`), 369,785 CO samples (`co_samples`), 50µs interval, deterministic.
- **Byte COUNT is run-dependent** (fill count moves with the order stream). Committed runs:
  **25,743,624** (representative, `bytes`), 25,759,344, and 25,797,720
  (`verified_runs/aftab/live_replay.txt`, 200k/200k, 309,962 fills).
- **Rule (`byte_exact_replay.note`):** cite 25,743,624 as *representative*, never as a fixed
  constant. Lead with the identical-match claim.
- **Why it matters:** the gold standard cannot disagree with itself — *that* is what makes
  contestant scoring trustworthy.
- **The planted bug it catches** (`correctness.planted_bug`): LIFO matching instead of FIFO
  price-time priority — a real HFT bug exchanges have shipped. The buggy journal is the
  **same length** (852 bytes) as the correct one, so a threshold checker passes it. The
  byte-exact diff catches it at **byte 244** (A=0x01 B=0x05), message #7, order_seq 1 (correct)
  vs 5 (bug).

### 1.5 Correctness, memory safety, concurrency, scale — the supporting wins

| Win | Value | canonical key / source |
|---|---|---|
| Unit tests | **19/19 passed** | `correctness.unit_tests` (19/19); `RESULTS.md §1` |
| Memory safety | **0 ASan / 0 UBSan findings** | `memory_safety.asan_ubsan_findings` = 0; `RESULTS.md §7` |
| SPSC lock-free offload | **dropped 0** over 300k (CI asserts every push) | `spsc_offload` (dropped 0, 300k/300k); `RESULTS.md §5` |
| Fleet merge | top score **35.67**, max-of-percentiles never averaged, 600k/600k | `fleet_merge`; `RESULTS.md §6` |
| Concurrency abuse | **32/32** bots on 4 cores (8× oversubscription), **1,279,594/1,279,594** sent=acked, 0 aborts, 0 collisions | `scale_32bot`; `RESULTS.md §8` |
| Thousands-scale | **2,989 connected** (3000 requested), 3,616,681 sent / 3,616,637 acked (within 44), 0 collisions, 0 double-counts | `scale_thousands`; `verified_runs/aftab/scale_3000bots.txt` |

**Scale-claim discipline (critical — `scale_thousands.note`):** label the 2989-bot run
**exactly** as "scale + accounting under backpressure", **never** a flat "Sent==Acked"
latency claim. Under extreme oversubscription the bot **throttles** (PoolExhausted=3,727,008,
EAGAIN) rather than lose/double-count. Backpressure made visible, not hidden. The clean
1000+-per-node *latency* run is the distributed Phase 2.3 (not yet run — see §6 status).

---

## 2. THE ARCHITECTURE — three components, one frozen contract

The seam = `contracts/interface_contract_v1.h` — a **frozen, versioned (v1.1)** binary
wire format. One field change breaks three codebases, so it was frozen on day one. That
freeze is what let three people build in parallel without integration hell.

```
   FROZEN WIRE CONTRACT (interface_contract_v1.h)  — frozen v1.1, all 3 compile against it
            │
   [T1] SANDBOX (sandbox/)        — Firecracker microVM: real kernel + scheduling boundary
            │  constant-rate load, SBE binary wire
   [T2] BOT FLEET + TELEMETRY     — open-loop, CPU-pinned C++ load gen; CO-corrected tail;
        (bot-engine/)               gold reference engine; byte-exact diff
            │  per-second HDR + diffs, scored
   [T3] LEADERBOARD (frontend/)   — display-only; never recomputes a percentile → can't lie
```

### 2.1 Component LLD anchors (for Manish to expand from the design doc)

- **Component 1 — Sandbox** (`sandbox/`, design doc §4): pipeline
  `INTAKE → BUILD → ATTEST → PACK → ORCHESTRATE`. Firecracker, **not** containers, because
  we measure *latency* (containers share host scheduler → pollute the tail). Build *does*
  use Docker `--network none` (filesystem/network isolation is the right tool there).
  - **Contestant front door** (`platform/intake-api/`, Go + Redis queue): `POST /api/v1/submissions`
    → state machine `RECEIVED→BUILDING→ATTESTED→RUNNING→SCORED` (any stage → REJECTED).
    Honest status: worker advances to **ATTESTED** today; RUNNING/SCORED need a `/dev/kvm` host.
  - **Seccomp boundary is REAL + PROVEN** (Phase 1.4): static PID-1 `seccomp_init.c`,
    no-new-privs + 82-syscall BPF allowlist (default=KILL). Aftab recorded in a real microVM:
    3 malicious fixtures KILLED (exitcode 0x1f=SIGSYS, `__seccomp_filter` in panic trace);
    GOOD submission SURVIVES (clean exit 0x0). Evidence: `verified_runs/aftab/firecracker_*`,
    `firecracker_1.4.cast`. **(Note: `open_items` TODO still says "soften seccomp to
    specified/in-progress" — that TODO is STALE now that 1.4 is built+proven. Manish: use the
    PROVEN framing, drop the softening TODO.)**
- **Component 2a — Reference matching engine** (`order_book.h`, design doc §6): header-only
  (offline replay CLI + in-sandbox TCP server share identical logic). 4 invariants:
  single-writer per symbol, deterministic (never iterate the hash map), price-time priority
  (sparse `std::map` keyed by price tick — O(active-levels) memory, avoids OOM at distributed scale; byte-exact determinism re-verified in CI + `std::list` FIFO), maker-before-taker fill emission.
- **Component 2b — Bot fleet + telemetry** (`bot.cpp`, design doc §7, the centerpiece):
  open-loop, CPU-pinned, CO correction (Tene/Snyder), integrity self-gate, SPSC offload,
  additive HDR merge.
- **Component 3 — Leaderboard + gateway** (design doc §8): display-only; scoring is a pure
  function `0.40*latency + 0.30*throughput + 0.30*correctness`, min-max per protocol board,
  **hard-fail cap** (correctness failure caps score — speed never buys back incorrectness);
  per-protocol boards never mixed; integrity gate = **exclusion, not penalty**.

### 2.2 The frozen contract (design doc §5)

`[FrameHeader 4B][Payload N]`; 5 fixed-layout msg types (NewOrder/Cancel/Ack/Fill/Reject).
Three choices that pay off everywhere: (1) common header at identical offsets in all 5
types → telemetry extracts seq/timestamp/symbol without knowing the type; (2) fixed-point
`int64` tick prices, never float (float is non-deterministic → would break byte-exact);
(3) `seq` is the latency join key (`ack.ts − NewOrder.ts` for the same seq).

---

## 3. THE FOUR PROBLEM INSIGHTS (design doc §2 — the "we understood it" story)

These four are *the design*; everything else follows from them. Judges reward "engineering
excellence / how we understood the problem" highest — lead the design doc here.

1. **Coordinated Omission.** A naive bot stalls when the engine stalls, so it never takes
   the samples that should have landed during the stall → reports p99=30µs when reality was
   5ms. Present in a huge fraction of public benchmarks. CO-blind ranking ranks engines wrong.
2. **microVM, not container.** Containers share the host scheduler/kernel → a noisy neighbour
   shows up as the contestant's tail latency. For a *latency* benchmark that's fatal. microVM
   = own kernel, own scheduling domain.
3. **Per-protocol, never mixed.** Binary/WS/FIX/REST are not comparable latency populations
   (framing alone adds tens of µs before the engine sees a byte). Ranking REST vs binary on
   raw p99 is a category error.
4. **Percentiles are not averageable.** The only correct fleet p99 comes from **merging raw
   HDR histograms** and reading the percentile off the merged distribution. Forces bots to
   emit mergeable histograms, and the merge must be exact (HDR is provably additive).

---

## 4. THE DIFFERENTIATORS / MOAT (what makes us win vs a "broad and fast" team)

The pitch: most teams build "broad and fast" — a pipeline, a leaderboard, some numbers.
We picked the one bug that invalidates almost every public latency benchmark and made the
honest measurement of it the spine. Concretely, the moat is:

1. **CO correction** — the rare, hard part of honest latency measurement. Provable
   (both histograms shown side-by-side), 19.7×–76× gap demonstrated. §1.3.
2. **Byte-exact correctness** — first-divergent-byte diff, catches a same-length LIFO bug
   at byte 244. No heuristics, no thresholds. §1.4.
3. **Measured 7.7µs** — real hardware number, not a target; "design target" labels retired.
   §1.2.
4. **Four real protocols incl. FIX 4.4** — the protocol the brief names, measured end-to-end
   through the *real* reference engine with Sent==Acked. §1.1.
5. **Proven seccomp + microVM isolation** — malicious fixtures actually KILLED in a real
   microVM (SIGSYS), good submission survives. Not "specified" — proven. §2.1.
6. **Honesty discipline as a structural property** — display-only leaderboard that *cannot*
   compute a lie; integrity gate that excludes rather than penalises; every number traces to
   a committed `verified_runs/` log + `canonical.json`. We show "(pending)" rather than a
   synthetic number dressed as real.

### 4.1 The 11 ADRs (design doc §9 — "multiple approaches & trade-offs", judge rubric #2 & #3)

ADR-1 CO-corrected vs naive · ADR-2 Firecracker vs container · ADR-3 Docker `--network none`
build vs host build · ADR-4 binary SBE vs JSON/gRPC · ADR-5 frozen v1.1 vs living interface ·
ADR-6 merge raw HDR vs average p99s · ADR-7 byte-exact hard-fail cap vs threshold checker ·
ADR-8 single-clock RTT + CO vs cross-machine PTP · ADR-9 display-only vs frontend recompute ·
ADR-10 integrity gate = exclude vs penalise · ADR-11 `int64` ticks vs `double`.

### 4.2 Deliberate deferrals (design doc §9.1 — "decisions, not gaps")

AVX-512/SIMD (payoff at ~1M msg/s, we're ~10k/s/bot) · AF_XDP (matters <1µs receive; we're
at 7.7µs) · eBPF kernel prober (measures a *different* quantity than trader-visible RTT) ·
Aeron/Chronicle (wrong topology: N-to-1 fan-in, not 1-to-many) · HW NIC timestamp cmsg parse
(needs PTP NIC) · macOS CPU pinning (Linux-only API, degrades gracefully). Each carries the
scale threshold at which it would matter — "we knew exactly where the line was," not "ran out
of time."

---

## 5. THE HONESTY DISCIPLINE (the brand — weave through every section)

- Every headline number → committed `verified_runs/` evidence + `canonical.json` key.
- "(Roadmap)"/"(pending)"/"NOT YET RUN" labels used wherever a run isn't committed —
  WS/FIX/REST labels came *off* only after committed Sent==Acked HDR landed.
- Deliberately-hostile measurement environment (shared container, no isolation) for the
  headline runs — "a benchmark that shows beautiful numbers in a noisy environment is broken;
  this one refuses to."
- macOS caveat stated (Linux-only pthread affinity / SO_BUSY_POLL are no-ops).
- Scale run labeled "scale + accounting under backpressure", never a latency claim.
- Byte count framed as run-dependent; lead with IDENTICAL-match invariant.

### 5.1 Hostile-Judge Q&A (design doc §15 — keep, but FIX Q5)

Q1 no kernel bypass (deliberate deferral, ordering unchanged) · Q2 anti-gaming (3 layers:
attestation + integrity gate + byte-exact) · Q3 Firecracker vs containers · Q4 clock sync
(single-clock RTT, nothing to sync) · **Q5 WS/REST boards — STALE: rewrite.** Q5 still says
WS/REST are `(Roadmap)` / "not yet producing committed measured runs." That is no longer
true — WS 223µs / FIX 890µs / REST 2,404µs are measured, committed, labels removed. Manish:
rewrite Q5 to "all four boards are real and measured; here is the honest CI-runner caveat on
absolute values vs the robust ordering claim."

---

## 6. STATUS MATRIX — done vs pending (state every item precisely)

### DONE + COMMITTED + VERIFIED
- 4 real protocol benchmarks (binary 7.7µs / WS 223µs / FIX 890µs / REST 2,404µs),
  x86-CI-verified Sent==Acked.
- Measured bare-metal p99 7.7µs (isolcpus, i7-13620H).
- CO proof 19.7×–76×; byte-exact IDENTICAL/exit-0; 19/19 tests; 0 ASan/UBSan;
  SPSC dropped 0; fleet merge; 32-bot abuse; 2989-bot scale-accounting.
- Seccomp boundary REAL + PROVEN in real microVM (3 killed, good survives).
- 4 resilience clips (engine-kill, gateway-kill self-heal, integrity-gate exclusion,
  node-kill reschedule via k3d).
- WS/REST live boards wired; cross-protocol chart in RESULTS.md.
- Design doc + 11 ADRs + Performance Characteristics + Contestant Upload Flow + §15 Q&A.
- Single-node k3s deploy verified; k3d multi-node; GHCR images live; terraform applies
  (3 libvirt VMs); Cloudflare public-URL capability.

### PENDING (state honestly, never imply done)
- **3-machine 30k distributed run (Phase 2.3)** — THE remaining rank-1 checklist item;
  needs Aftab + Manish + Mohit on Tailscale together. This is the run that reports a large
  concurrent count *and* honest per-bot latency. Not run → not claimed anywhere.
- **FIX live board** — last small gateway/frontend wiring (FIX is measured+charted, just not
  on the live bar yet). Branch `fix-live-board`.
- **Demo video (5.1)** — 5–7 min full live loop, once everything's up.
- **Docs regen (5.2)** — design doc → shareable Google Doc; refresh .docx (Manish).
- Optional: crisp good-submission Firecracker re-capture; €5 bare-metal server row.

---

## 7. MAPPING HINTS → the 19-section judge format (Manish's finalize step)

Manish has the exact 19-section template; these are the content-to-section hints based on the
rubric priorities already encoded in design doc §0:

| Judge rubric priority | Source blocks in THIS draft |
|---|---|
| 1. Engineering excellence / problem understanding | §3 (four insights), §2 (architecture), §1.3 (CO centerpiece) |
| 2. Product decisions | §2.1 component anchors, §4 differentiators |
| 3. Multiple approaches & trade-offs | §4.1 (11 ADRs), §4.2 (deferrals) |
| 4. End-to-end thinking | §2 data flow, §6 deployment status |
| 5. Deploy-ready code | §6 DONE (IaC, k3s, GHCR, terraform) |
| 6. Tests performed & verified | §1 (every number, sourced), §5 honesty discipline |

**Three things Manish must fix during finalize (consistency sweep):**
1. Design doc §15 **Q5** — rewrite (WS/REST no longer Roadmap). See §5.1 above.
2. `canonical.json` `open_items` last TODO ("soften seccomp to specified/in-progress") — now
   STALE; 1.4 is built+proven. Use the PROVEN framing.
3. Re-grep all surfaces for any residual "design target", "35×", "PTP LOCKED", "(Roadmap)" on
   WS/REST — must be zero (QA gate).

---

## 8. SOUNDBITES (drop-in lines, all source-backed)

- "The hard part of a latency benchmark is not drawing the chart — it's not lying in the chart."
- "Anyone can draw a leaderboard. We built the one that refuses to show you a number it can't
  stand behind."
- "19.7× tabulated, up to 76× on isolated hardware — the cleaner the host, the harder naive lies."
- "p99 = 7.7µs, measured, not targeted — on an isolated consumer desktop, not server hardware."
- "Binary ≪ WebSocket ≪ FIX ≪ REST — and FIX 4.4, the protocol the brief names, lands exactly
  where theory says."
- "A same-length LIFO bug passes a threshold checker; our byte-exact diff catches it at byte 244."
- "Three malicious fixtures killed in a real microVM with SIGSYS; the good submission survives."
