#!/usr/bin/env node
/* ============================================================================
 * fix_adapter.js — FIX 4.4 → reference-engine adapter.
 *
 * Terminates a FIX 4.4 session from fix_bot and translates it to the real
 * reference engine over the binary SBE wire — like rest_adapter, but FIX:
 *   Logon (35=A)          → reply Logon
 *   NewOrderSingle (35=D) → SBE NewOrder → refserver → OrderAck → ExecutionReport (35=8)
 * ClOrdID (tag 11) is the correlation key, echoed back on the ExecutionReport.
 * Real engine, real FIX text framing (SOH + BodyLength + CheckSum) — the cost
 * the cross-protocol chart exists to expose.
 *
 * Usage: node fix_adapter.js --port 9003 --engine-host 127.0.0.1 --engine-port 9000
 * ========================================================================== */
'use strict';
const net = require('net');
const SOH = '\x01';

const args = process.argv.slice(2);
const arg = (n, d) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : d; };
const PORT        = parseInt(arg('--port', '9003'), 10);
const ENGINE_HOST = arg('--engine-host', '127.0.0.1');
const ENGINE_PORT = parseInt(arg('--engine-port', '9000'), 10);
const MSG_NEWORDER = 1, MSG_ORDERACK = 3;

function frameFix(body) {                      // body = fields from tag 35, each SOH-terminated
  const hdr = '8=FIX.4.4' + SOH + '9=' + Buffer.byteLength(body) + SOH;
  const m = hdr + body;
  let sum = 0; for (let i = 0; i < m.length; i++) sum += m.charCodeAt(i);
  return m + '10=' + String(sum % 256).padStart(3, '0') + SOH;
}
function fixField(msg, tag) {
  const k = SOH + tag + '=';
  let p = msg.indexOf(k);
  if (p < 0) { if (msg.startsWith(tag + '=')) p = 0; else return ''; } else p += 1;
  const eq = msg.indexOf('=', p), s = msg.indexOf(SOH, eq);
  return (eq < 0 || s < 0) ? '' : msg.slice(eq + 1, s);
}
function buildNewOrder(seq) {                   // SBE FrameHeader(4)+NewOrder(40)
  const b = Buffer.alloc(44);
  b.writeUInt8(MSG_NEWORDER, 0); b.writeUInt16LE(40, 2);
  b.writeBigUInt64LE(BigInt(seq), 4);           // NewOrder.seq
  b.writeBigUInt64LE(BigInt(Date.now()) * 1000000n, 12);
  b.writeUInt32LE(1, 20);                        // symbol
  b.writeUInt8(0, 24); b.writeUInt8(0, 25);     // type=LIMIT, side=BUY
  b.writeBigInt64LE(10000n, 28);                // price
  b.writeBigUInt64LE(10n, 36);                  // qty
  return b;
}

const server = net.createServer((fix) => {
  fix.setNoDelay(true);
  let fbuf = '';
  let execSeq = 1;
  const eng = net.connect(ENGINE_PORT, ENGINE_HOST); eng.setNoDelay(true);
  let ebuf = Buffer.alloc(0);

  // engine → bot: SBE OrderAck → FIX ExecutionReport (35=8), ClOrdID echoed
  eng.on('data', (chunk) => {
    ebuf = Buffer.concat([ebuf, chunk]);
    while (ebuf.length >= 4) {
      const msgLen = ebuf.readUInt16LE(2), total = 4 + msgLen;
      if (ebuf.length < total) break;
      if (ebuf.readUInt8(0) === MSG_ORDERACK && msgLen >= 32) {
        const clordid = ebuf.readBigUInt64LE(28).toString();   // OrderAck.order_seq @ off 28
        const body = '35=8' + SOH + '49=ENGINE' + SOH + '56=BOT' + SOH + '34=' + (execSeq++) + SOH +
                     '11=' + clordid + SOH + '17=' + clordid + SOH + '150=0' + SOH + '39=0' + SOH +
                     '55=1' + SOH + '54=1' + SOH + '38=10' + SOH + '14=0' + SOH + '6=0' + SOH;
        if (fix.writable) fix.write(frameFix(body));
      }
      ebuf = ebuf.subarray(total);
    }
  });

  // bot → engine: parse FIX messages; Logon → reply Logon, NewOrderSingle → SBE
  fix.on('data', (d) => {
    fbuf += d.toString('binary');
    for (;;) {
      const s = fbuf.indexOf('8=FIX'); if (s < 0) break;
      const t = fbuf.indexOf(SOH + '10=', s);
      if (t < 0 || fbuf.length < t + 1 + 3 + 3 + 1) break;
      const end = t + 1 + 3 + 3 + 1;
      const msg = fbuf.slice(s, end); fbuf = fbuf.slice(end);
      const mt = fixField(msg, '35');
      if (mt === 'A') {
        fix.write(frameFix('35=A' + SOH + '49=ENGINE' + SOH + '56=BOT' + SOH + '34=1' + SOH + '98=0' + SOH + '108=30' + SOH));
      } else if (mt === 'D') {
        const seq = fixField(msg, '11');
        if (seq && eng.writable) eng.write(buildNewOrder(seq));
      }
    }
  });

  const close = () => { try { fix.destroy(); } catch (e) {} try { eng.destroy(); } catch (e) {} };
  fix.on('close', close); fix.on('error', close);
  eng.on('close', close); eng.on('error', close);
});

server.listen(PORT, () =>
  console.log(`[fix_adapter] FIX :${PORT}  →  engine ${ENGINE_HOST}:${ENGINE_PORT} (FIX↔SBE, real engine)`));
server.on('error', (e) => { console.error('[fix_adapter] listen error:', e.message); process.exit(1); });
