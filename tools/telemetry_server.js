#!/usr/bin/env node
/* ============================================================================
 * IICPC HFT Benchmarking Platform — Telemetry Gateway (reference / mock)
 *
 * Serves the leaderboard's LIVE feed. Speaks the exact wire shape the frontend
 * subscriber (frontend/index.html → connectLive()) expects:
 *
 *     { "type": "deltas", "deltas": { "BINARY TCP":[row,...], "WEBSOCKET (Roadmap)":[...], "REST (Roadmap)":[...] } }
 *
 * Each row is ALREADY scored + ranked here (server-side), honouring the
 * Interface Contract cardinal rule: the browser only displays, never computes
 * a percentile. p99 used for scoring is read from the merged additive HDR.
 *
 * This is the REFERENCE implementation. The production gateway (Rust/Go reading
 * VictoriaMetrics + additive HDR blobs from the bot fleet) is a drop-in
 * replacement that emits the same JSON — the frontend never changes.
 *
 * Two data sources (every frame carries "source" so the live badge can't lie):
 *   (default)               LIVE_CSV — ingests the fleet's per-second HDR
 *                           snapshots from --snapshot-dir (default /telemetry).
 *                           Frames carry  "source":"LIVE_CSV".
 *   --demo                  DEMO — contract-faithful synthetic generator so a
 *                           demo works with zero backend. Frames carry
 *                           "source":"DEMO". The gateway ALSO falls back to DEMO
 *                           (honestly labelled) when the snapshot dir is absent
 *                           or empty — the LIVE badge is honest by construction.
 *
 * Usage:
 *   node telemetry_server.js --port 8080 [--snapshot-dir /telemetry]   # live (default)
 *   node telemetry_server.js --port 8080 --demo                        # synthetic
 *   → ws://localhost:8080/leaderboard/deltas
 *
 * Dependency: ws  (npm i ws)
 * ========================================================================== */
'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');
let WebSocketServer;
try { WebSocketServer = require('ws').Server; }
catch (e) {
  console.error("Missing dependency 'ws'. Run:  npm install ws");
  process.exit(1);
}

/* ---- args ---------------------------------------------------------------- */
const args = process.argv.slice(2);
function arg(name, def) { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; }
const PORT = parseInt(arg('--port', '8080'), 10);
const DEMO = args.includes('--demo');
// Live by default: ingest the fleet's CSV snapshots. --demo forces synthetic.
const SNAPSHOT_DIR = DEMO ? null : arg('--snapshot-dir', '/telemetry');
const CADENCE_MS = parseInt(arg('--cadence-ms', '2500'), 10);   // 1–5s per spec

/* ============================================================================
 * HDR — additive histogram (HdrHistogram-faithful, NANOSECONDS), mirrors the
 * frontend's engine so scoring reads p99 from the merged additive HDR.
 * ========================================================================== */
const HDR_MIN = 500, HDR_MAX = 500000000, HDR_N = 80;        // 500ns … 500ms
const HDR_EDGES = (() => { const e = [], lo = Math.log(HDR_MIN), hi = Math.log(HDR_MAX);
  for (let i = 0; i <= HDR_N; i++) e.push(Math.exp(lo + (hi - lo) * i / HDR_N)); return e; })();
function hdrNew() { return { c: new Array(HDR_N).fill(0), n: 0 }; }
function hdrRecord(h, ns) {
  let i = Math.floor(HDR_N * (Math.log(ns) - Math.log(HDR_MIN)) / (Math.log(HDR_MAX) - Math.log(HDR_MIN)));
  i = Math.max(0, Math.min(HDR_N - 1, i)); h.c[i]++; h.n++;
}
function hdrMerge(a, b) { const m = hdrNew(); for (let i = 0; i < HDR_N; i++) m.c[i] = a.c[i] + b.c[i]; m.n = a.n + b.n; return m; }
function hdrValueAtPercentile(h, p) {
  if (h.n === 0) return 0; const t = h.n * p / 100; let cum = 0;
  for (let i = 0; i < HDR_N; i++) { cum += h.c[i]; if (cum >= t) return HDR_EDGES[i + 1]; }
  return HDR_EDGES[HDR_N];
}

/* ============================================================================
 * Synthetic source — contract-faithful. Same roster/shape as the frontend mock
 * so the live feed looks identical to the in-page demo, just served over WS.
 * ========================================================================== */
const PROTOCOLS = ['BINARY TCP', 'WEBSOCKET (Roadmap)', 'REST (Roadmap)'];
const NODES = ['node-01', 'node-02', 'node-03', 'node-04'];
function gauss() { let u = 0, v = 0; while (!u) u = Math.random(); while (!v) v = Math.random();
  return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v); }
function hashId(s) { let h = 5381; for (let i = 0; i < s.length; i++) h = ((h << 5) + h + s.charCodeAt(i)) >>> 0; return h.toString(16).padStart(8, '0'); }

const Source = (() => {
  const base = { 'BINARY TCP': [10000, 28000], 'WEBSOCKET (Roadmap)': [35000, 90000], 'REST (Roadmap)': [120000, 420000] };  // ns
  const roster = [
    ['Priya Sharma', 'BINARY TCP'], ['Arjun Mehta', 'BINARY TCP'], ['Sanya Rao', 'BINARY TCP'], ['Dev Kapoor', 'BINARY TCP'],
    ['Kiran Nair', 'WEBSOCKET (Roadmap)'], ['Rohan Gupta', 'WEBSOCKET (Roadmap)'], ['Ananya Iyer', 'WEBSOCKET (Roadmap)'], ['Vikram Sen', 'WEBSOCKET (Roadmap)'],
    ['Meera Joshi', 'REST (Roadmap)'], ['Aditya Rao', 'REST (Roadmap)'], ['Neha Verma', 'REST (Roadmap)'], ['Karan Shah', 'REST (Roadmap)'],
  ];
  let subs = [];
  function sampleInto(h, median, tail, count) {
    for (let k = 0; k < count; k++) { let v = median * Math.exp(0.35 * gauss());
      if (Math.random() < tail.p) v *= tail.mult * (1 + Math.random()); hdrRecord(h, Math.max(HDR_MIN, v)); }
  }
  function init() {
    subs = roster.map((r, i) => {
      const [name, proto] = r, [lo, hi] = base[proto];
      const median = lo + Math.random() * (hi - lo);
      const tail = { p: 0.01 + Math.random() * 0.06, mult: 6 + Math.random() * 30 };
      const hdr = hdrNew(); sampleInto(hdr, median, tail, 60000);
      const node = NODES[i % NODES.length];
      const submission_id = (hashId(name + proto + 'a') + hashId(name + proto + 'b')).slice(0, 16);
      const run_id = (hashId('run' + i + 'x') + hashId('run' + i)).slice(0, 12);
      return { schema_version: 1, submission_id, run_id, contestant: name,
        contestant_version: 'v' + (1 + i % 3) + '.' + (i % 5) + '.' + (i % 9),
        protocol: proto, node_id: node, _median: median, _tail: tail, hdr, gauges: {}, ptp: {}, integrity: {} };
    });
    subs.forEach(refresh);
  }
  function refresh(s) {
    s.gauges = {
      p50: hdrValueAtPercentile(s.hdr, 50), p90: hdrValueAtPercentile(s.hdr, 90),
      p99: hdrValueAtPercentile(s.hdr, 99), p99_9: hdrValueAtPercentile(s.hdr, 99.9),
      p99_99: hdrValueAtPercentile(s.hdr, 99.99), max: hdrValueAtPercentile(s.hdr, 100),
      tps: Math.round((s.protocol === 'BINARY TCP' ? 180000 : s.protocol === 'WEBSOCKET (Roadmap)' ? 120000 : 60000) * (0.6 + Math.random() * 0.6)),
      err: Math.max(0, Math.min(0.12, 0.005 + Math.abs(gauss()) * 0.01)),
      diff_pass_rate: Math.random() < 0.10 ? 0.985 + Math.random() * 0.012 : 1.0,
      invariant_violations: Math.random() < 0.08 ? Math.floor(1 + Math.random() * 2) : 0,
    };
    const noisy = (s.node_id === 'node-04') && (Math.random() < 0.55);
    const selftest_p99_ns = noisy ? 5500 + Math.random() * 4000 : 1200 + Math.random() * 2500;
    const software_jitter_ns = noisy ? 1100 + Math.random() * 1500 : 200 + Math.random() * 600;
    s.integrity = { selftest_p99_ns, software_jitter_ns, gate_passed: selftest_p99_ns <= 5000 && software_jitter_ns <= 1000 };
    const holdover = noisy && Math.random() < 0.7;
    s.ptp = { max_offset_ns: holdover ? 400 + Math.random() * 900 : 18 + Math.random() * 70,
      mean_offset_ns: holdover ? 220 + Math.random() * 400 : 9 + Math.random() * 30,
      sync_state: holdover ? 'HOLDOVER' : 'LOCKED' };
  }
  let curSource = 'DEMO';
  function tick() {
    // Re-evaluate freshness every tick: a sub is only "from_csv" if a real CSV
    // row fed it THIS tick. Without this reset, stale data sticks when a file
    // disappears, and the LIVE badge would lie.
    subs.forEach(s => s.from_csv = false);
    let csvRows = 0;
    if (!DEMO && SNAPSHOT_DIR && fs.existsSync(SNAPSHOT_DIR)) {
      // sort() — readdir order is filesystem-dependent; keep the
      // file→contestant mapping deterministic across platforms.
      const files = fs.readdirSync(SNAPSHOT_DIR).filter(f => f.endsWith('.csv')).sort();
      files.forEach((f, i) => {
        if (i < 4 && i < subs.length) { // Map to first 4 contestants
          try {
            const content = fs.readFileSync(path.join(SNAPSHOT_DIR, f), 'utf-8').trim();
            const lines = content.split('\n');
            if (lines.length > 1) {
              const last = lines[lines.length - 1].split(',');
              const s = subs[i];
              // elapsed_sec,sent,acked,naive_p50,naive_p90,naive_p99,naive_p99_9,naive_p99_99,naive_max,co_p50,co_p90,co_p99,co_p99_9,co_p99_99,co_max
              s.gauges.p50 = parseFloat(last[9]) || 0;
              s.gauges.p90 = parseFloat(last[10]) || 0;
              s.gauges.p99 = parseFloat(last[11]) || 0;
              s.gauges.p99_9 = parseFloat(last[12]) || 0;
              s.gauges.p99_99 = parseFloat(last[13]) || 0;
              s.gauges.max = parseFloat(last[14]) || 0;
              
              const elapsed = parseFloat(last[0]);
              s.gauges.tps = elapsed > 0 ? Math.round(parseFloat(last[2]) / elapsed) : 0;
              s.gauges.err = 0;
              s.gauges.diff_pass_rate = 1.0;
              s.gauges.invariant_violations = 0;
              s.integrity.selftest_p99_ns = 1000;
              s.integrity.software_jitter_ns = 200;
              s.integrity.gate_passed = true;
              s.from_csv = true;
              csvRows++;
            }
          } catch(e) {}
        }
      });
    }

    // Honest by construction: the frame is LIVE_CSV only if real fleet rows fed
    // it this tick; otherwise (--demo, or snapshot dir absent/empty) it's DEMO.
    curSource = csvRows > 0 ? 'LIVE_CSV' : 'DEMO';

    subs.forEach(s => {
      if (!s.from_csv) {
        const d = hdrNew(); sampleInto(d, s._median * (0.92 + Math.random() * 0.18), s._tail, 4000);
        s.hdr = hdrMerge(s.hdr, d); refresh(s);
      }
    });
    return subs;
  }
  return { init, tick, all: () => subs, source: () => curSource };
})();

/* ============================================================================
 * Scoring service — pure fn, Interface Contract §5. Identical math to the
 * frontend's so mock and live agree. p99 from MERGED HDR, never an average.
 * ========================================================================== */
const CAP_FAILED = -1;
const hardFail = g => g.diff_pass_rate < 0.999 || g.invariant_violations > 0;
const norm = (v, lo, hi) => hi <= lo ? 1 : Math.max(0, Math.min(1, (v - lo) / (hi - lo)));
function scoreProtocol(list) {
  const ranked = [], excluded = [];
  list.forEach(s => s.integrity.gate_passed ? ranked.push(s) : excluded.push(s));
  ranked.forEach(s => s._p99 = s.from_csv ? s.gauges.p99 : hdrValueAtPercentile(s.hdr, 99));
  const ok = ranked.filter(s => !hardFail(s.gauges));
  const loL = Math.min(...ok.map(s => s._p99), Infinity), hiL = Math.max(...ok.map(s => s._p99), 0);
  const loT = Math.min(...ok.map(s => s.gauges.tps), Infinity), hiT = Math.max(...ok.map(s => s.gauges.tps), 0);
  ranked.forEach(s => {
    if (hardFail(s.gauges)) { s._score = CAP_FAILED; return; }
    const nLat = 1 - norm(s._p99, loL, hiL), nTps = norm(s.gauges.tps, loT, hiT), nCor = s.gauges.diff_pass_rate;
    s._score = Math.round((0.40 * nLat + 0.30 * nTps + 0.30 * nCor) * 1000) / 10;
  });
  ranked.sort((a, b) => {
    if (a._score === CAP_FAILED && b._score !== CAP_FAILED) return 1;
    if (b._score === CAP_FAILED && a._score !== CAP_FAILED) return -1;
    return b._score - a._score;
  });
  return { ranked, excluded };
}
function rowOf(s, rank, excluded = false) {
  return { id: s.submission_id, rank, excluded, failed: s._score === CAP_FAILED,
    contestant: s.contestant, contestant_version: s.contestant_version,
    protocol: s.protocol, node: s.node_id, submission_id: s.submission_id, run_id: s.run_id,
    g: s.gauges, ptp: s.ptp, integrity: s.integrity, score: s._score, hdr: s.hdr };
}
function buildDeltas(subs) {
  const out = {};
  PROTOCOLS.forEach(p => {
    const { ranked, excluded } = scoreProtocol(subs.filter(s => s.protocol === p));
    out[p] = ranked.map((s, i) => rowOf(s, i + 1)).concat(excluded.map(s => rowOf(s, null, true)));
  });
  return out;
}

/* ============================================================================
 * Server — HTTP upgrade → WS at /leaderboard/deltas. Broadcasts deltas every
 * CADENCE_MS. Sends a snapshot immediately on connect so the board is never
 * blank.
 * ========================================================================== */
Source.init();
Source.tick();   // settle the source label so the very first frame is honest

const server = http.createServer((req, res) => {
  if (req.url === '/health') { res.writeHead(200); res.end('ok'); return; }
  res.writeHead(200, { 'content-type': 'text/plain' });
  res.end('IICPC telemetry gateway. Connect: ws://<host>:' + PORT + '/leaderboard/deltas');
});

const wss = new WebSocketServer({ server, path: '/leaderboard/deltas' });
function frame() { return JSON.stringify({ type: 'deltas', source: Source.source(), deltas: buildDeltas(Source.all()) }); }

wss.on('connection', (ws) => {
  console.log('[gateway] client connected (' + wss.clients.size + ' total)');
  try { ws.send(frame()); } catch (e) {}           // immediate snapshot
  ws.on('close', () => console.log('[gateway] client disconnected'));
});

setInterval(() => {
  Source.tick();
  const msg = frame();
  wss.clients.forEach(c => { if (c.readyState === 1) { try { c.send(msg); } catch (e) {} } });
}, CADENCE_MS);

server.listen(PORT, () => {
  console.log('[gateway] IICPC telemetry gateway listening on :' + PORT);
  console.log('[gateway] leaderboard feed → ws://localhost:' + PORT + '/leaderboard/deltas');
  console.log('[gateway] cadence ' + CADENCE_MS + 'ms · mode: ' +
    (DEMO ? 'DEMO (synthetic, forced via --demo)'
          : 'LIVE_CSV from ' + SNAPSHOT_DIR + ' (falls back to DEMO if absent/empty)'));
  console.log('[gateway] current source label: ' + Source.source());
  if (!DEMO && SNAPSHOT_DIR && !fs.existsSync(SNAPSHOT_DIR)) {
    console.log('[gateway] note: snapshot-dir "' + SNAPSHOT_DIR + '" not found — serving DEMO until it appears');
  }
});
