#!/bin/bash
# attest.sh — Attestation stage (§7 stage 3)
#
# The "Knight Capital rule": verify contestant ELF identity before
# any code executes. Two checks:
#   1. ELF must respond to --version with a version string
#   2. Compute SHA-256 of the ELF and store in attestation manifest
#
# Per doc §7 stage 3:
#   contestant ELF must expose /version (Knight Capital rule)
#   hash the ELF on EVERY target node; mismatch → abort run
#   (defends the "differs across nodes" failure class)
#
# Usage:
#   ./attest.sh <contestant.elf> [--output <manifest.json>]
#
# Output:
#   - Writes attestation manifest to stdout or --output file
#   - Exits 0 on success, non-zero on failure

set -euo pipefail

ELF_PATH=""
OUTPUT_FILE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output) OUTPUT_FILE="$2"; shift 2 ;;
        -*)       echo "ERROR: Unknown option: $1" >&2; exit 1 ;;
        *)        ELF_PATH="$1"; shift ;;
    esac
done

if [[ -z "$ELF_PATH" ]]; then
    echo "ERROR: No ELF path provided." >&2
    echo "Usage: $0 <contestant.elf> [--output <manifest.json>]" >&2
    exit 1
fi

if [[ ! -f "$ELF_PATH" ]]; then
    echo "ATTEST FAILED: File not found: $ELF_PATH" >&2
    exit 1
fi

if [[ ! -x "$ELF_PATH" ]]; then
    echo "ATTEST FAILED: File not executable: $ELF_PATH" >&2
    exit 1
fi

# ── Check 1: --version endpoint ─────────────────────────────
# Run with a 5-second timeout to prevent hanging
VERSION_OUTPUT=$(timeout 5 "$ELF_PATH" --version 2>/dev/null || true)

if [[ -z "$VERSION_OUTPUT" ]]; then
    echo "ATTEST FAILED: ELF does not respond to --version (Knight Capital rule)" >&2
    echo "  The contestant binary MUST print a version string when invoked with --version" >&2
    exit 1
fi

# Trim whitespace
VERSION_STRING=$(echo "$VERSION_OUTPUT" | head -1 | tr -d '\r' | xargs)
echo "[Attest] Version: $VERSION_STRING" >&2

# ── Check 2: Compute SHA-256 hash ──────────────────────────
ELF_HASH=$(sha256sum "$ELF_PATH" | awk '{print $1}')
ELF_SIZE=$(stat -c%s "$ELF_PATH")
echo "[Attest] SHA-256: $ELF_HASH" >&2
echo "[Attest] Size: $ELF_SIZE bytes" >&2

# ── Generate attestation manifest ──────────────────────────
MANIFEST=$(cat <<EOF
{
    "attestation_version": 1,
    "elf_path": "$ELF_PATH",
    "elf_sha256": "$ELF_HASH",
    "elf_size_bytes": $ELF_SIZE,
    "version_string": "$VERSION_STRING",
    "attested_at": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
    "attested_on_host": "$(hostname)"
}
EOF
)

if [[ -n "$OUTPUT_FILE" ]]; then
    echo "$MANIFEST" > "$OUTPUT_FILE"
    echo "[Attest] Manifest written to: $OUTPUT_FILE" >&2
else
    echo "$MANIFEST"
fi

echo "[Attest] ✓ Attestation passed" >&2
