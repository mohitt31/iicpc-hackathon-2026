#!/usr/bin/env node
/* ============================================================================
 * rest_adapter.js — REST/HTTP → reference-engine adapter.
 *
 * Same purpose as ws_adapter: measure the SAME real engine over a DIFFERENT
 * transport, so the framing/encoding overhead is exposed (binary ≪ WS ≪ REST).
 * Unlike WS (a transparent byte-proxy), REST needs translation: rest_bot POSTs
 * a small JSON order, this adapter converts it to the SBE NewOrder wire bytes,
 * forwards to the real refserver over TCP, reads the SBE OrderAck, and replies
 * with the JSON ack. Real engine, real HTTP+JSON overhead — nothing synthetic.
 *
 *   rest_bot ──POST /orders {json}──► rest_adapter ──TCP(SBE NewOrder)──► refserver
 *            ◄──200 {"order_seq","status":"ACK"}──            ◄──TCP(OrderAck)──
 *
 * JSON mapping (documented in rest_bot.cpp): {seq,ts,symbol,type,side,price,qty}.
 * Wire frame = FrameHeader(4) + NewOrder(40): msg_type@0=1, msg_len@2=40,
 *   seq@4, ts@12, symbol@20, type@24, side@25, price@28, qty@36 (little-endian).
 * OrderAck (msg_type 3): order_seq at frame offset 4+24 = 28.
 *
 * Usage:  node rest_adapter.js --port 9002 --engine-host 127.0.0.1 --engine-port 9000
 * ========================================================================== */
'use strict';
const http = require('http');
const net  = require('net');

const args = process.argv.slice(2);
const arg = (n, d) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : d; };
const PORT        = parseInt(arg('--port', '9002'), 10);
const ENGINE_HOST = arg('--engine-host', '127.0.0.1');
const ENGINE_PORT = parseInt(arg('--engine-port', '9000'), 10);

const MSG_NEWORDER = 1, MSG_ORDERACK = 3;

function buildNewOrder(o) {
  const b = Buffer.alloc(44);
  b.writeUInt8(MSG_NEWORDER, 0);                 // FrameHeader.msg_type
  b.writeUInt8(0, 1);                            // _pad
  b.writeUInt16LE(40, 2);                        // FrameHeader.msg_len = sizeof(NewOrder)
  b.writeBigUInt64LE(BigInt(o.seq || 0), 4);
  b.writeBigUInt64LE(BigInt(o.ts || 0), 12);
  b.writeUInt32LE(o.symbol || 1, 20);
  b.writeUInt8(o.type || 0, 24);
  b.writeUInt8(o.side || 0, 25);
  b.writeBigInt64LE(BigInt(o.price || 0), 28);
  b.writeBigUInt64LE(BigInt(o.qty || 0), 36);
  return b;
}

// One engine TCP connection per HTTP keep-alive socket, with a frame parser
// that resolves the JSON response when the matching OrderAck arrives.
function engineFor(httpSocket, onReady) {
  if (httpSocket._engine) return onReady(httpSocket._engine);
  const eng = net.connect(ENGINE_PORT, ENGINE_HOST);
  eng.setNoDelay(true);
  eng._buf = Buffer.alloc(0);
  eng._pending = new Map();                       // order_seq -> http res
  eng.on('data', (chunk) => {
    eng._buf = Buffer.concat([eng._buf, chunk]);
    while (eng._buf.length >= 4) {
      const msgLen = eng._buf.readUInt16LE(2);
      const total = 4 + msgLen;
      if (eng._buf.length < total) break;
      const msgType = eng._buf.readUInt8(0);
      if (msgType === MSG_ORDERACK && msgLen >= 32) {
        const orderSeq = eng._buf.readBigUInt64LE(28).toString();   // OrderAck.order_seq @ frame off 28
        const res = eng._pending.get(orderSeq);
        if (res) {
          eng._pending.delete(orderSeq);
          res.writeHead(200, { 'Content-Type': 'application/json' });
          res.end('{"order_seq":' + orderSeq + ',"status":"ACK"}');
        }
      }
      eng._buf = eng._buf.subarray(total);          // drain Fills / others
    }
  });
  eng.on('error', () => { try { httpSocket.destroy(); } catch (e) {} });
  eng.on('close', () => { httpSocket._engine = null; });
  httpSocket._engine = eng;
  httpSocket.on('close', () => { try { eng.destroy(); } catch (e) {} });
  onReady(eng);
}

const server = http.createServer((req, res) => {
  if (req.method !== 'POST' || !req.url.startsWith('/orders')) { res.writeHead(404); res.end(); return; }
  let body = '';
  req.on('data', (d) => { body += d; if (body.length > 4096) req.destroy(); });
  req.on('end', () => {
    let o; try { o = JSON.parse(body); } catch (e) { res.writeHead(400); res.end('bad json'); return; }
    engineFor(req.socket, (eng) => {
      eng._pending.set(String(o.seq), res);          // resolved when OrderAck(seq) returns
      eng.write(buildNewOrder(o));
    });
  });
});

server.listen(PORT, () =>
  console.log(`[rest_adapter] HTTP :${PORT}  →  engine ${ENGINE_HOST}:${ENGINE_PORT} (JSON↔SBE, real engine)`));
server.on('error', (e) => { console.error('[rest_adapter] listen error:', e.message); process.exit(1); });
