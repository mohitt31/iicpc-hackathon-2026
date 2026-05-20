#!/bin/bash
# verify_on_node.sh — Per-node ELF hash verification (§7 stage 3 continued)
#
# Before spawning a microVM on any target node, this script re-hashes
# the contestant ELF and compares against the attestation manifest.
# Mismatch → abort the run.
#
# Per doc §7 stage 3:
#   hash the ELF on EVERY target node; mismatch → abort run
#   (defends the "differs across nodes" failure class)
#
# Usage:
#   ./verify_on_node.sh <contestant.elf> <attestation_manifest.json>
#
# Exit codes:
#   0 — hash matches, safe to proceed
#   1 — hash mismatch, ABORT

set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <contestant.elf> <attestation_manifest.json>" >&2
    exit 1
fi

ELF_PATH="$1"
MANIFEST_PATH="$2"

if [[ ! -f "$ELF_PATH" ]]; then
    echo "VERIFY FAILED: ELF not found: $ELF_PATH" >&2
    exit 1
fi

if [[ ! -f "$MANIFEST_PATH" ]]; then
    echo "VERIFY FAILED: Manifest not found: $MANIFEST_PATH" >&2
    exit 1
fi

# ── Extract expected hash from manifest ─────────────────────
# Use grep+sed for portability (jq may not be on every node)
EXPECTED_HASH=$(grep '"elf_sha256"' "$MANIFEST_PATH" | sed 's/.*: *"\([^"]*\)".*/\1/')

if [[ -z "$EXPECTED_HASH" ]]; then
    echo "VERIFY FAILED: Could not extract elf_sha256 from manifest" >&2
    exit 1
fi

# ── Compute actual hash ────────────────────────────────────
ACTUAL_HASH=$(sha256sum "$ELF_PATH" | awk '{print $1}')

# ── Compare ─────────────────────────────────────────────────
if [[ "$ACTUAL_HASH" == "$EXPECTED_HASH" ]]; then
    echo "[Verify] ✓ Hash match on $(hostname)"
    echo "  Expected: $EXPECTED_HASH"
    echo "  Actual:   $ACTUAL_HASH"
    exit 0
else
    echo "VERIFY FAILED: Hash mismatch on $(hostname)!" >&2
    echo "  Expected: $EXPECTED_HASH" >&2
    echo "  Actual:   $ACTUAL_HASH" >&2
    echo "  ELF may have been tampered with or corrupted during transfer." >&2
    echo "  ABORTING RUN." >&2
    exit 1
fi
