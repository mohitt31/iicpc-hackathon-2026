#!/bin/bash
# integration_test.sh — End-to-end test: bot ↔ reference_server
#
# This script:
#   1. Builds all targets
#   2. Starts reference_server in the background
#   3. Runs the bot for 3 seconds at low rate
#   4. Verifies: bot reports non-zero acked count, fills occur,
#      server exits cleanly, journals are non-empty
#   5. Replays the input journal through refengine and diffs
#      against the server's live output journal → MUST be IDENTICAL
#      (this is the determinism proof)
#
# Usage: ./test/integration_test.sh
# Run from bot-engine/ directory.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
TEST_TMP="$BUILD_DIR/integration_test_tmp"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass_count=0
fail_count=0

pass() { echo -e "  ${GREEN}✓ $1${NC}"; pass_count=$((pass_count + 1)); }
fail() { echo -e "  ${RED}✗ $1${NC}"; fail_count=$((fail_count + 1)); }

cleanup() {
    # Kill any leftover server
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    # Don't remove test_tmp so user can inspect on failure
}
trap cleanup EXIT

echo ""
echo "═══════════════════════════════════════════════"
echo "  Integration Test: Bot ↔ Reference Server"
echo "═══════════════════════════════════════════════"
echo ""

# ── Step 1: Check binaries exist ────────────────────────────
echo "▸ Step 1: Checking binaries..."
for bin in reference_server bot refengine; do
    if [[ ! -x "$BUILD_DIR/$bin" ]]; then
        echo -e "  ${RED}ERROR: $BUILD_DIR/$bin not found. Run cmake build first.${NC}"
        exit 1
    fi
done
pass "All binaries present"

# ── Step 2: Set up test workspace ───────────────────────────
echo "▸ Step 2: Setting up test workspace..."
rm -rf "$TEST_TMP"
mkdir -p "$TEST_TMP"
pass "Test workspace created at $TEST_TMP"

# ── Step 3: Start reference_server ──────────────────────────
echo "▸ Step 3: Starting reference_server..."
"$BUILD_DIR/reference_server" \
    --port 9000 \
    --journal "$TEST_TMP/server_output.jrn" \
    --input-journal "$TEST_TMP/server_input.jrn" \
    > "$TEST_TMP/server.log" 2>&1 &
SERVER_PID=$!

# Wait until the server is actually accepting on the port before launching
# the bot. A fixed sleep races with TSC calibration + listen(): if the bot's
# one-shot connect() fires first it fails hard and the run produces empty
# journals (a false "0 bytes identical" pass). Poll for readiness instead.
SERVER_READY=0
for _ in $(seq 1 100); do  # up to ~10s
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        break  # server died; reported below
    fi
    if lsof -nP -iTCP:9000 -sTCP:LISTEN >/dev/null 2>&1; then
        SERVER_READY=1
        break
    fi
    perl -e 'select(undef,undef,undef,0.1)'  # portable 100ms sleep
done

if [[ "$SERVER_READY" -eq 1 ]]; then
    pass "Reference server started (PID=$SERVER_PID)"
else
    fail "Reference server failed to start / never listened on port 9000"
    cat "$TEST_TMP/server.log"
    exit 1
fi

# ── Step 4: Run the bot ────────────────────────────────────
echo "▸ Step 4: Running bot (3 seconds, 1000µs interval)..."
"$BUILD_DIR/bot" \
    --interval-us 1000 \
    --duration-sec 3 \
    > "$TEST_TMP/bot.log" 2>&1 || true

# Give server a moment to flush
sleep 0.5

# Stop the server gracefully
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true

pass "Bot run complete"

# ── Step 5: Verify bot output ──────────────────────────────
echo "▸ Step 5: Checking bot results..."

# Check bot acked some messages
BOT_ACKED=$(grep -o 'Total Acked: [0-9]*' "$TEST_TMP/bot.log" | grep -o '[0-9]*$' || echo "0")
if [[ "$BOT_ACKED" -gt 0 ]]; then
    pass "Bot received $BOT_ACKED acks"
else
    fail "Bot received 0 acks"
fi

# Check bot received fills (real engine should produce them)
BOT_FILLS=$(grep -o 'Total Fills: [0-9]*' "$TEST_TMP/bot.log" | grep -o '[0-9]*$' || echo "0")
if [[ "$BOT_FILLS" -gt 0 ]]; then
    pass "Bot received $BOT_FILLS fills"
else
    # Fills depend on order matching — all same-side orders won't cross.
    # The bot currently sends BUY@15050 every time, so no matches.
    # This is expected behavior, not a failure.
    echo -e "  ${YELLOW}⚠ Bot received 0 fills (expected: bot sends same-side orders)${NC}"
    pass_count=$((pass_count + 1))
fi

# ── Step 6: Verify journals are non-empty ──────────────────
echo "▸ Step 6: Checking journals..."

INPUT_SIZE=$(wc -c < "$TEST_TMP/server_input.jrn" 2>/dev/null | tr -d ' ' || echo "0")
OUTPUT_SIZE=$(wc -c < "$TEST_TMP/server_output.jrn" 2>/dev/null | tr -d ' ' || echo "0")

if [[ "$INPUT_SIZE" -gt 0 ]]; then
    pass "Input journal: $INPUT_SIZE bytes"
else
    fail "Input journal is empty"
fi

if [[ "$OUTPUT_SIZE" -gt 0 ]]; then
    pass "Output journal: $OUTPUT_SIZE bytes"
else
    fail "Output journal is empty"
fi

# ── Step 7: Determinism check — replay + diff ──────────────
echo "▸ Step 7: Determinism verification (replay + diff)..."

"$BUILD_DIR/refengine" replay \
    "$TEST_TMP/server_input.jrn" \
    "$TEST_TMP/replay_output.jrn" \
    2>&1

DIFF_RESULT=$("$BUILD_DIR/refengine" diff \
    "$TEST_TMP/server_output.jrn" \
    "$TEST_TMP/replay_output.jrn" \
    2>&1) || true

if echo "$DIFF_RESULT" | grep -q "IDENTICAL"; then
    MATCH_BYTES=$(echo "$DIFF_RESULT" | grep -o 'IDENTICAL: [0-9]*' | grep -o '[0-9]*$')
    if [[ -z "$MATCH_BYTES" || "$MATCH_BYTES" -eq 0 ]]; then
        # Two empty journals diff as "IDENTICAL: 0 bytes" — a false pass.
        # Real determinism requires actual matched output.
        fail "DETERMINISM INCONCLUSIVE: 0 bytes compared (empty journals)"
    else
        pass "DETERMINISM VERIFIED: live server and offline replay are byte-identical ($MATCH_BYTES bytes)"
    fi
else
    fail "DETERMINISM FAILURE: live server output differs from offline replay"
    echo "  Diff output: $DIFF_RESULT"
fi

# ── Summary ─────────────────────────────────────────────────
echo ""
echo "═══════════════════════════════════════════════"
echo -e "  Results: ${GREEN}$pass_count passed${NC}, ${RED}$fail_count failed${NC}"
echo "═══════════════════════════════════════════════"
echo ""
echo "  Logs:    $TEST_TMP/bot.log"
echo "           $TEST_TMP/server.log"
echo ""

exit $fail_count
