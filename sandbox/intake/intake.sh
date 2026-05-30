#!/bin/bash
# intake.sh — Submission intake stage (§7 stage 1)
#
# Accepts a contestant submission tarball, validates it, and produces
# a submission_id (sha256 hash) for downstream stages.
#
# Checks:
#   1. File exists and is readable
#   2. File size is under the cap (default 50MB)
#   3. Source files do not contain banned syscall patterns
#
# Usage:
#   ./intake.sh <tarball_path> [--size-cap-mb <MB>] [--output-dir <dir>]
#
# Output:
#   - Extracts tarball to <output_dir>/<submission_id>/src/
#   - Prints submission_id to stdout on success
#   - Exits non-zero with error message on rejection
#
# Per doc §7 stage 1:
#   tarball/binary uploaded → sha256 → submission_id (immutable identity)
#   → reject if > size cap or banned syscalls in static scan

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BANNED_FILE="$SCRIPT_DIR/banned_syscalls.txt"

# ── Defaults ────────────────────────────────────────────────
SIZE_CAP_MB=50
OUTPUT_DIR=""
TARBALL=""

# ── Parse args ──────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --size-cap-mb) SIZE_CAP_MB="$2"; shift 2 ;;
        --output-dir)  OUTPUT_DIR="$2";  shift 2 ;;
        -*)            echo "ERROR: Unknown option: $1" >&2; exit 1 ;;
        *)             TARBALL="$1";      shift ;;
    esac
done

if [[ -z "$TARBALL" ]]; then
    echo "ERROR: No tarball path provided." >&2
    echo "Usage: $0 <tarball_path> [--size-cap-mb <MB>] [--output-dir <dir>]" >&2
    exit 1
fi

if [[ -z "$OUTPUT_DIR" ]]; then
    OUTPUT_DIR="$(dirname "$TARBALL")/submissions"
fi

# ── Check 1: File exists ────────────────────────────────────
if [[ ! -f "$TARBALL" ]]; then
    echo "REJECTED: File not found: $TARBALL" >&2
    exit 1
fi

if [[ ! -r "$TARBALL" ]]; then
    echo "REJECTED: File not readable: $TARBALL" >&2
    exit 1
fi

# ── Check 2: Size cap ──────────────────────────────────────
SIZE_CAP_BYTES=$((SIZE_CAP_MB * 1024 * 1024))
FILE_SIZE=$(stat -c%s "$TARBALL" 2>/dev/null || stat -f%z "$TARBALL")

if [[ "$FILE_SIZE" -gt "$SIZE_CAP_BYTES" ]]; then
    echo "REJECTED: File size ${FILE_SIZE} bytes exceeds cap of ${SIZE_CAP_BYTES} bytes (${SIZE_CAP_MB}MB)" >&2
    exit 1
fi

# ── Compute submission_id (sha256) ──────────────────────────
SUBMISSION_ID=$(sha256sum "$TARBALL" | awk '{print $1}')

# ── Extract ─────────────────────────────────────────────────
SUBMISSION_DIR="$OUTPUT_DIR/$SUBMISSION_ID"
mkdir -p "$SUBMISSION_DIR/src"

tar xf "$TARBALL" -C "$SUBMISSION_DIR/src" 2>/dev/null || {
    echo "REJECTED: Failed to extract tarball (corrupt or unsupported format)" >&2
    rm -rf "$SUBMISSION_DIR"
    exit 1
}

# ── Check 3: Banned syscall scan ────────────────────────────
if [[ ! -f "$BANNED_FILE" ]]; then
    echo "WARNING: Banned syscalls file not found at $BANNED_FILE, skipping scan" >&2
else
    VIOLATIONS=""
    # Scan all C/C++ source files
    while IFS= read -r pattern; do
        # Skip comments and empty lines
        [[ "$pattern" =~ ^[[:space:]]*# ]] && continue
        [[ -z "${pattern// /}" ]] && continue

        # Search for the pattern as a word boundary in source files
        FOUND=$(grep -rlw --include='*.c' --include='*.cpp' --include='*.cc' \
                --include='*.h' --include='*.hpp' --include='*.cxx' \
                "$pattern" "$SUBMISSION_DIR/src" 2>/dev/null || true)

        if [[ -n "$FOUND" ]]; then
            VIOLATIONS="${VIOLATIONS}\n  Banned pattern '${pattern}' found in:\n"
            while IFS= read -r f; do
                # Show relative path for clarity
                REL="${f#$SUBMISSION_DIR/src/}"
                VIOLATIONS="${VIOLATIONS}    - ${REL}\n"
            done <<< "$FOUND"
        fi
    done < "$BANNED_FILE"

    if [[ -n "$VIOLATIONS" ]]; then
        echo -e "REJECTED: Banned syscall patterns detected:${VIOLATIONS}" >&2
        rm -rf "$SUBMISSION_DIR"
        exit 1
    fi
fi

# ── Record metadata ─────────────────────────────────────────
cat > "$SUBMISSION_DIR/intake_metadata.json" <<EOF
{
    "submission_id": "$SUBMISSION_ID",
    "original_file": "$(basename "$TARBALL")",
    "file_size_bytes": $FILE_SIZE,
    "intake_timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
    "size_cap_mb": $SIZE_CAP_MB,
    "scanned_patterns": "$(wc -l < "$BANNED_FILE" | tr -d ' ')"
}
EOF

# ── Success ─────────────────────────────────────────────────
echo "$SUBMISSION_ID"
