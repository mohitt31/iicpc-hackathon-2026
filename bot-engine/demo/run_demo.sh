#!/bin/bash
# run_demo.sh — Live demo of correctness validator catching a planted bug.
#
# Story (for the stage):
#   "We have a reference matching engine — the gold standard for what
#    correct behavior looks like. We also have a contestant's engine
#    that looks plausible. Both ack. Both fill. Both produce reasonable
#    output. But our diff tool finds the bug in seconds."
#
# Run from bot-engine/:
#   ./demo/run_demo.sh

set -e

cd "$(dirname "$0")/.."

# ── Sanity check binaries ────────────────────────────────────────────────
if [ ! -x build/refengine ];     then echo "ERROR: build/refengine missing.     Run cmake build first."; exit 1; fi
if [ ! -x build/buggy_engine ];  then echo "ERROR: build/buggy_engine missing.  Run cmake build first."; exit 1; fi

# ── Cleanup previous run ─────────────────────────────────────────────────
rm -f /tmp/demo_input.jrn /tmp/demo_correct.jrn /tmp/demo_buggy.jrn

echo "════════════════════════════════════════════════════════════════════"
echo "  DEMO: Correctness Validator vs Planted Price-Time Priority Bug"
echo "════════════════════════════════════════════════════════════════════"
echo ""

# ── Step 1: Generate input scenario ──────────────────────────────────────
echo "[1/4] Generating demo input scenario..."
python3 demo/make_demo_journal.py /tmp/demo_input.jrn
echo ""

# ── Step 2: Run reference engine (correct) ───────────────────────────────
echo "[2/4] Running REFERENCE engine (gold standard, FIFO matching)..."
./build/refengine replay /tmp/demo_input.jrn /tmp/demo_correct.jrn
ls -la /tmp/demo_correct.jrn
echo ""

# ── Step 3: Run buggy engine ─────────────────────────────────────────────
echo "[3/4] Running BUGGY engine (contestant's submission, LIFO bug)..."
./build/buggy_engine replay /tmp/demo_input.jrn /tmp/demo_buggy.jrn
ls -la /tmp/demo_buggy.jrn
echo ""

# Quick visual sanity — both files are the same SIZE (same number of msgs)
# but their contents differ. Looks fine at a glance!
echo "Both files are the same size — at a glance, both engines look correct:"
echo "  correct: $(wc -c < /tmp/demo_correct.jrn | awk '{print $1}') bytes"
echo "  buggy:   $(wc -c < /tmp/demo_buggy.jrn | awk '{print $1}') bytes"
echo ""
echo "Now let the diff tool prove otherwise..."
echo ""

# ── Step 4: Run diff tool — THE MONEY SHOT ───────────────────────────────
echo "[4/4] Running BYTE-EXACT DIFF (this is the validator that scores contestants)..."
echo "════════════════════════════════════════════════════════════════════"
./build/refengine diff /tmp/demo_correct.jrn /tmp/demo_buggy.jrn || true
echo "════════════════════════════════════════════════════════════════════"
echo ""
echo "→ The diff tool pinpointed the exact Fill where price-time priority"
echo "  was violated. This is the validator we score every contestant with."
echo ""
