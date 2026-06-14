#!/usr/bin/env node
/* ============================================================================
 * ws_adapter.js — WebSocket → reference-engine transport adapter.
 *
 * The point of the per-protocol boards is to measure the SAME engine over
 * DIFFERENT transports, so the framing overhead (binary ≪ WS ≪ REST) is the
 * teaching moment. This adapter is a transparent byte-proxy: it terminates the
 * RFC-6455 WebSocket from ws_bot and pipes the binary-frame payload — which IS
 * the SBE FrameHeader+NewOrder wire bytes — straight to the real reference
 * engine over TCP, then wraps the engine's ack bytes back into WS binary frames.
 *
 * So what ws_bot measures is: real engine latency + real WebSocket framing/
 * masking overhead. Nothing synthetic — the engine is the gold-standard
 * refserver, the only added cost is the transport, which is exactly the cost we
 * are trying to expose.
 *
 *   ws_bot  ──WS(masked binary, SBE payload)──►  ws_adapter  ──TCP(SBE)──►  refserver
 *           ◄──WS(binary, SBE ack)──────────────             ◄──TCP(ack)──
 *
 * Usage:  node ws_adapter.js --port 9001 --engine-host 127.0.0.1 --engine-port 9000
 * Dep:    ws
 * ========================================================================== */
'use strict';
const net = require('net');
let WebSocketServer;
try { WebSocketServer = require('ws').Server; }
catch (e) { console.error("Missing dependency 'ws'. Run: npm install ws"); process.exit(1); }

const args = process.argv.slice(2);
const arg = (n, d) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : d; };
const PORT        = parseInt(arg('--port', '9001'), 10);
const ENGINE_HOST = arg('--engine-host', '127.0.0.1');
const ENGINE_PORT = parseInt(arg('--engine-port', '9000'), 10);

const wss = new WebSocketServer({ port: PORT });

wss.on('connection', (ws) => {
  // one dedicated TCP connection to the engine per bot connection
  const tcp = net.connect(ENGINE_PORT, ENGINE_HOST);
  tcp.setNoDelay(true);

  // bot → engine: the WS binary payload is the raw SBE bytes; forward verbatim
  ws.on('message', (data, isBinary) => {
    if (tcp.writable) tcp.write(data);
  });
  // engine → bot: stream the engine's ack bytes back as WS binary frames
  tcp.on('data', (chunk) => {
    if (ws.readyState === ws.OPEN) ws.send(chunk, { binary: true });
  });

  const close = () => { try { ws.close(); } catch (e) {} try { tcp.destroy(); } catch (e) {} };
  ws.on('close', close);
  ws.on('error', close);
  tcp.on('close', close);
  tcp.on('error', (e) => { console.error('[ws_adapter] engine TCP error:', e.message); close(); });
});

wss.on('listening', () =>
  console.log(`[ws_adapter] WS :${PORT}  →  engine ${ENGINE_HOST}:${ENGINE_PORT} (transparent SBE byte-proxy)`));
wss.on('error', (e) => { console.error('[ws_adapter] listen error:', e.message); process.exit(1); });
