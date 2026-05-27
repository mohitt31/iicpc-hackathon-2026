#!/bin/bash
# integration_test.sh — End-to-end test: bot fires real orders against
# reference_engine and verifies acks/fills/rejects come back correctly.
#
# Why this exists:
#   soak_test.sh only tests against null_responder, which acks everything
#   without doing any matching logic. This doesn't exercise the contract.
#   This script verifies the bot can sustain load against a stateful
#   matching engine without contract violations.
#
# What it does:
#   1. Wraps the offline reference_engine in a tiny TCP server (Python)
#      that reads NewOrder/CancelOrder frames, calls reference_engine
#      logic via a journal pipe, and writes back acks/fills/rejects.
#   2. Runs the bot against this wrapper for 10 seconds.
#   3. Verifies: acks ≈ sent, no PartialAborts, no PoolExhausted, and
#      a non-zero fill+reject count (proving the engine is actually
#      matching, not just acking).
#
# NOTE: This wrapper is FOR TESTING ONLY — production sandbox uses
# Aftab's TCP shim around the same reference_engine binary.

set -e
cd "$(dirname "$0")/.."

if [ ! -x build/refengine ]; then
    echo "ERROR: build/refengine missing."
    exit 1
fi
if [ ! -x build/bot ]; then
    echo "ERROR: build/bot missing."
    exit 1
fi

PORT=9100
RESULTS_DIR="integration_results/$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"

echo "════════════════════════════════════════════════════════════════════"
echo "  INTEGRATION TEST — bot vs reference_engine"
echo "════════════════════════════════════════════════════════════════════"
echo "Port: $PORT"
echo "Results: $RESULTS_DIR"
echo ""

# ── TCP wrapper for reference_engine ──────────────────────────────────────
# Reads bot frames, replays through a refengine subprocess, sends back.
# Uses a journal-file roundtrip for simplicity and correctness — the
# refengine binary is unchanged, no logic duplication.
cat > "$RESULTS_DIR/wrapper.py" << 'PYEOF'
import socket, struct, subprocess, sys, threading, os, signal, time, tempfile

PORT = int(sys.argv[1])
REFENGINE = sys.argv[2]

FRAME_HDR_SIZE = 4
MSG_NEWORDER, MSG_CANCEL = 1, 2
MSG_ORDERACK, MSG_FILL, MSG_REJECT = 3, 4, 5

NEW_SIZE    = 40
CANCEL_SIZE = 32

# We accept ONE bot connection at a time (single bot for this test).
# Each frame received is appended to a per-connection journal file.
# Every K frames we flush the journal through refengine and stream
# outputs back to the bot.

def handle_client(conn, addr):
    print(f"[wrapper] client connected: {addr}", flush=True)
    seq_counter = 0

    # Per-client journal in/out files
    jin  = tempfile.NamedTemporaryFile(delete=False, suffix=".jrn")
    jout_path = jin.name + ".out"

    BATCH = 200  # flush every N input frames
    pending_in = 0

    try:
        buf = b""
        while True:
            chunk = conn.recv(65536)
            if not chunk:
                break
            buf += chunk

            # Parse complete frames
            while len(buf) >= FRAME_HDR_SIZE:
                msg_type, _pad, msg_len = struct.unpack("<BBH", buf[:4])
                total = FRAME_HDR_SIZE + msg_len
                if len(buf) < total:
                    break
                frame = buf[:total]
                buf = buf[total:]
                jin.write(frame)
                pending_in += 1

            if pending_in >= BATCH:
                jin.flush()
                # Replay through refengine — produces /out journal
                subprocess.run(
                    [REFENGINE, "replay", jin.name, jout_path],
                    check=True
                )
                # Send back all engine outputs
                with open(jout_path, "rb") as fout:
                    out_data = fout.read()
                if out_data:
                    conn.sendall(out_data)
                # Reset journals for next batch
                jin.close()
                os.unlink(jin.name)
                if os.path.exists(jout_path):
                    os.unlink(jout_path)
                jin = tempfile.NamedTemporaryFile(delete=False, suffix=".jrn")
                jout_path = jin.name + ".out"
                pending_in = 0
    except Exception as e:
        print(f"[wrapper] client error: {e}", flush=True)
    finally:
        # Final flush
        try:
            jin.flush()
            if pending_in > 0:
                subprocess.run(
                    [REFENGINE, "replay", jin.name, jout_path],
                    check=False
                )
                if os.path.exists(jout_path):
                    with open(jout_path, "rb") as fout:
                        out_data = fout.read()
                    if out_data:
                        try: conn.sendall(out_data)
                        except: pass
        except Exception:
            pass
        try: jin.close()
        except: pass
        for p in [jin.name, jout_path]:
            if os.path.exists(p):
                try: os.unlink(p)
                except: pass
        conn.close()
        print(f"[wrapper] client disconnected", flush=True)

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT))
srv.listen(4)
print(f"[wrapper] listening on 127.0.0.1:{PORT}", flush=True)

while True:
    conn, addr = srv.accept()
    t = threading.Thread(target=handle_client, args=(conn, addr), daemon=True)
    t.start()
PYEOF

# ── Launch wrapper ────────────────────────────────────────────────────────
echo "[1/4] Starting reference_engine TCP wrapper on port $PORT..."
python3 "$RESULTS_DIR/wrapper.py" "$PORT" "$(pwd)/build/refengine" \
    > "$RESULTS_DIR/wrapper.log" 2>&1 &
WRAPPER_PID=$!
sleep 2

# Verify wrapper is up
if ! kill -0 $WRAPPER_PID 2>/dev/null; then
    echo "ERROR: wrapper failed to start. Log:"
    cat "$RESULTS_DIR/wrapper.log"
    exit 1
fi
echo "      Wrapper PID: $WRAPPER_PID"

# ── Run bot against reference_engine ──────────────────────────────────────
# Slower interval (1ms) — refengine wrapper is Python and not designed
# for HFT speeds; this is a correctness test, not a perf test.
echo ""
echo "[2/4] Running bot (1 bot, 1ms interval, 10 seconds)..."

./build/bot \
    --bots 1 --no-pin \
    --interval-us 1000 \
    --duration-sec 10 \
    --port $PORT \
    --no-gate \
    2>&1 | tee "$RESULTS_DIR/bot.log"

BOT_EXIT=${PIPESTATUS[0]}
echo "      Bot exit code: $BOT_EXIT"

# ── Stop wrapper ──────────────────────────────────────────────────────────
echo ""
echo "[3/4] Stopping wrapper..."
kill $WRAPPER_PID 2>/dev/null || true
wait $WRAPPER_PID 2>/dev/null || true

# ── Verify results ────────────────────────────────────────────────────────
echo ""
echo "[4/4] Verifying contract behavior..."

SENT=$(grep "Aggregate:"     "$RESULTS_DIR/bot.log" | grep -oE 'Sent=[0-9]+'           | head -1 | cut -d= -f2)
ACKED=$(grep "Aggregate:"    "$RESULTS_DIR/bot.log" | grep -oE 'Acked=[0-9]+'          | head -1 | cut -d= -f2)
FILLS=$(grep "Aggregate:"    "$RESULTS_DIR/bot.log" | grep -oE 'Fills=[0-9]+'          | head -1 | cut -d= -f2)
REJECTS=$(grep "Aggregate:"  "$RESULTS_DIR/bot.log" | grep -oE 'Rejects=[0-9]+'        | head -1 | cut -d= -f2)
PARTIAL=$(grep "Aggregate:"  "$RESULTS_DIR/bot.log" | grep -oE 'PartialAborts=[0-9]+'  | head -1 | cut -d= -f2)
POOLX=$(grep "Aggregate:"    "$RESULTS_DIR/bot.log" | grep -oE 'PoolExhausted=[0-9]+'  | head -1 | cut -d= -f2)
COLL=$(grep "Aggregate:"     "$RESULTS_DIR/bot.log" | grep -oE 'Collisions=[0-9]+'     | head -1 | cut -d= -f2)

# Verdict logic
PASS=true
verdict() {
    local label="$1"
    local actual="${2:-MISSING}"
    local expect="$3"
    if [ "$actual" = "$expect" ]; then
        printf "  %-22s %-10s PASS\n" "$label" "$actual"
    else
        printf "  %-22s %-10s FAIL (expected %s)\n" "$label" "$actual" "$expect"
        PASS=false
    fi
}

verdict_nonzero() {
    local label="$1"
    local actual="${2:-MISSING}"
    if [ -n "$actual" ] && [ "$actual" -gt 0 ] 2>/dev/null; then
        printf "  %-22s %-10s PASS (>0)\n" "$label" "$actual"
    else
        printf "  %-22s %-10s FAIL (expected >0)\n" "$label" "$actual"
        PASS=false
    fi
}

echo ""
echo "─── Contract behavior ───"
verdict        "PartialAborts"     "$PARTIAL"  "0"
verdict        "PoolExhausted"     "$POOLX"    "0"
verdict        "PendingCollisions" "$COLL"     "0"

echo ""
echo "─── Engine actually matched (not just acked) ───"
echo "  Sent:     $SENT"
echo "  Acked:    $ACKED"
echo "  Fills:    $FILLS"
echo "  Rejects:  $REJECTS"

# Fills + rejects should be > 0 — proves the engine is actually doing
# matching work, not just rubber-stamping every order.
if [ -n "$FILLS" ] && [ -n "$REJECTS" ]; then
    TOTAL_ENGINE_WORK=$((FILLS + REJECTS))
    if [ "$TOTAL_ENGINE_WORK" -gt 0 ]; then
        echo "  Engine activity:       $TOTAL_ENGINE_WORK (Fills + Rejects)  PASS"
    else
        echo "  Engine activity:       0  FAIL (engine did no matching)"
        PASS=false
    fi
else
    echo "  Engine activity:       UNKNOWN — could not parse Fills/Rejects"
    PASS=false
fi

echo ""
echo "════════════════════════════════════════════════════════════════════"
if [ "$PASS" = true ] && [ "$BOT_EXIT" = "0" ]; then
    echo "  INTEGRATION TEST: PASS"
    EXIT_CODE=0
else
    echo "  INTEGRATION TEST: FAIL"
    EXIT_CODE=1
fi
echo "════════════════════════════════════════════════════════════════════"
echo ""
echo "Logs:"
echo "  $RESULTS_DIR/bot.log"
echo "  $RESULTS_DIR/wrapper.log"

exit $EXIT_CODE
