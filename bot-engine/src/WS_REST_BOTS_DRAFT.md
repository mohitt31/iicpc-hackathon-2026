# WS / REST bots — DRAFT status (Phase 3.2 / 3.3)

**These two files are UNVERIFIED drafts. Do not treat them as done.**

| File | Protocol | Phase | Status |
|---|---|---|---|
| `src/ws_bot.cpp`   | WebSocket (RFC-6455) | 3.2 | DRAFT — never compiled or run by the author |
| `src/rest_bot.cpp` | HTTP/1.1 REST        | 3.3 | DRAFT — never compiled or run by the author |

## What they are
Candidate implementations that reuse the TCP bot's measurement core — the SBE
`NewOrder` payload (`contracts/interface_contract_v1.h`), the intended-send-time
Coordinated-Omission logic, and the HDR naive+CO histogram pipeline — and swap only
the transport:
- **ws_bot**: wraps the SBE bytes in a masked RFC-6455 binary frame.
- **rest_bot**: maps the NewOrder fields to a documented JSON body, `POST /orders`,
  HTTP/1.1 keep-alive.

## What must happen before they count (and before "(Roadmap)" labels come off)
1. **Build green in CI on x86.** They are unbuilt; expect first-compile fixes.
2. **Run against a WS/REST-capable engine** (the reference engine currently speaks
   binary TCP — a WS/REST listener or adapter is required).
3. **Commit a real run log to `verified_runs/`** showing **Sent == Acked** at the
   chosen rate, plus the naive/CO p99 per protocol.
4. Only THEN remove the `(Roadmap)` labels from the WebSocket and REST boards and
   add the cross-protocol latency chart (binary << ws << REST) to RESULTS.md.

## Known open items (called out inline in each file too)
- **ws_bot:** RX ack-correlation is elided in the draft send loop (the TCP bot pairs
  acks by `order_seq` on the epoll RX side; the WS bot must do the same). Strict
  `Sec-WebSocket-Accept` validation is computed-but-not-enforced. No TLS (ws:// on the
  benchmark LAN). No RX fragmentation reassembly.
- **rest_bot:** one in-flight request per connection (synchronous RTT — the honest
  baseline for a latency comparison). No chunked transfer-encoding parse. The engine's
  actual `/orders` contract must match the documented mapping.

## Suggested CI build (for `release-images.yml` / Aftab)
```
g++ -O2 -std=c++17 -I. -Ibot-engine/src \
    bot-engine/src/ws_bot.cpp   -lhdr_histogram -o build/ws_bot
g++ -O2 -std=c++17 -I. -Ibot-engine/src \
    bot-engine/src/rest_bot.cpp -lhdr_histogram -o build/rest_bot
```
(adjust include/lib paths to match the existing botengine Dockerfile).
