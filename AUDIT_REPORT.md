# Code + Connection Audit — findings

_Audit of `mohitt31/iicpc-hackathon-2026` @ main. Scope: 4 protocol paths, gateway,
intake-api, CI, infra. Method: static trace against the contract + canonical.json._

## Verdict: the platform is wired coherently end-to-end. One overclaim found and fixed (#73).

### 4 protocol paths — CLEAN
- **ws_adapter.js** is a transparent SBE byte-proxy: one dedicated TCP connection to the
  engine per WS connection, WS binary payload forwarded verbatim, engine acks streamed
  back as WS binary frames. Correct.
- **fix_adapter.js** does real FIX 4.4 framing (`8=FIX.4.4` / BodyLength / CheckSum), with
  ClOrdID (tag 11) as the ack-correlation key. Builds the SBE NewOrder correctly.
- **rest_adapter.js** does documented JSON↔SBE translation, real HTTP+JSON overhead.
- **SBE byte offsets verified against `contracts/interface_contract_v1.h`:** the adapters
  write frame-absolute offsets (seq@4, ts@12, symbol@20, type@24, side@25, price@28,
  qty@36) which reconcile exactly with the contract's payload-relative offsets (+4 for the
  FrameHeader). No silent field corruption. This was the highest-risk check — a wrong
  offset would pass Sent==Acked while feeding the engine garbage. It is correct.

### Gateway (`tools/telemetry_server.js`) — CLEAN + honest
- Routes each fleet CSV to the right protocol board by filename (`ws_bot.csv`→WEBSOCKET,
  `rest_bot.csv`→REST, `fix_bot.csv`→FIX, `bot_*`→BINARY), `sort()`ed for cross-platform
  determinism.
- Source-honesty mechanism is real: `from_csv` is reset every tick, so the LIVE badge
  cannot stick when a CSV disappears; `LIVE_CSV` vs `DEMO` vs `MOCK` is enforced, not
  cosmetic.

### intake-api — CLEAN
- Full state machine RECEIVED→BUILDING→ATTESTED→RUNNING→SCORED (+REJECTED) present in
  `main.go`, exercised in `main_test.go`. Redis-backed queue.

### CI workflows — all 6 present
- `wsrest-build.yml` (compile + per-protocol Sent==Acked smokes), `compose-smoke.yml`,
  `intake-api.yml`, `release-images.yml`, `benchmark.yml`, `pages.yml`.

### infra — consistent
- `docker-compose.yml` and `infra/k8s/platform.yaml` agree: reference-engine →
  bot-fleet (DaemonSet) → telemetry-gateway → leaderboard, same GHCR image.

## The one real finding (FIXED in #73)
RESULTS.md claimed the WS/FIX/REST p99s "come from the committed CI smoke
(`wsrest-build.yml`)". They do not — that smoke asserts Sent==Acked but never captures a
p99, and no `verified_runs/` file contains 223/890/2404us. Reworded: loops are
CI-verified for INTEGRITY; absolute latencies are REPRESENTATIVE of the framing-overhead
ordering until a committed isolated-host latency run lands. canonical.json de-staled
(FIX-forbidding note, done TODOs, status).

## Still pending (hardware — Aftab's lane)
- Committed per-protocol latency run (isolated host) → promotes WS/FIX/REST from
  representative to measured.
- Multi-node ≥3-node distributed scale run. Turnkey script staged:
  `infra/run_distributed_scale.sh` (labels nodes → applies manifests → scales the fleet →
  captures `kubectl get pods -o wide` + per-pod accounting; refuses to run on <3 nodes so
  a single-node result can't masquerade as distributed).
