#!/bin/bash
# pipeline.sh — Full submission pipeline (§7 stages 1-4)
#
# Chains: INTAKE → BUILD → ATTEST → PACK
#
# Usage:
#   ./pipeline.sh <tarball_path> [--output-dir <dir>] [--skip-docker]
#
# Modes:
#   Default:       Uses Docker for hermetic build (§7 stage 2)
#   --skip-docker: Builds locally (for dev/testing without Docker)
#
# Output:
#   <output-dir>/<submission_id>/
#     ├── src/                    — extracted source
#     ├── intake_metadata.json    — from intake stage
#     ├── contestant.elf          — compiled binary
#     ├── run_metadata.json       — compiler info
#     ├── attestation.json        — hash + version
#     └── rootfs.ext4             — Firecracker image (if not skipping)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TARBALL=""
OUTPUT_DIR=""
SKIP_DOCKER=false
SKIP_PACK=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir)  OUTPUT_DIR="$2";  shift 2 ;;
        --skip-docker) SKIP_DOCKER=true; shift ;;
        --skip-pack)   SKIP_PACK=true;   shift ;;
        -*)            echo "ERROR: Unknown option: $1" >&2; exit 1 ;;
        *)             TARBALL="$1";      shift ;;
    esac
done

if [[ -z "$TARBALL" ]]; then
    echo "Usage: $0 <tarball_path> [--output-dir <dir>] [--skip-docker] [--skip-pack]" >&2
    exit 1
fi

if [[ -n "$OUTPUT_DIR" ]]; then
    INTAKE_ARGS="--output-dir $OUTPUT_DIR"
else
    INTAKE_ARGS=""
fi

echo ""
echo "═══════════════════════════════════════════════"
echo "  Submission Pipeline — §7 Stages 1-4"
echo "═══════════════════════════════════════════════"
echo ""

# ── Stage 1: INTAKE ─────────────────────────────────────────
echo "▸ Stage 1: INTAKE"
# shellcheck disable=SC2086
SUBMISSION_ID=$("$SCRIPT_DIR/intake/intake.sh" "$TARBALL" $INTAKE_ARGS)
echo "  submission_id: $SUBMISSION_ID"

if [[ -n "$OUTPUT_DIR" ]]; then
    SUBMISSION_DIR="$OUTPUT_DIR/$SUBMISSION_ID"
else
    SUBMISSION_DIR="$(dirname "$TARBALL")/submissions/$SUBMISSION_ID"
fi

echo "  workspace: $SUBMISSION_DIR"
echo ""

# ── Stage 2: BUILD ──────────────────────────────────────────
echo "▸ Stage 2: BUILD"

if [[ "$SKIP_DOCKER" == "true" ]]; then
    echo "  [skip-docker] Building locally..."

    # Find source files
    SRC_DIR="$SUBMISSION_DIR/src"
    SOURCES=$(find "$SRC_DIR" -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' -o -name '*.c' | head -50)

    if [[ -z "$SOURCES" ]]; then
        echo "ERROR: No source files found in $SRC_DIR" >&2
        exit 1
    fi

    COMPILER="g++"
    # On x86 hosts use march=x86-64-v2 (target judging environment).
    # On non-x86 hosts (Mac arm64) skip -march so local dev still builds.
    HOST_ARCH="$(uname -m)"
    if [[ "$HOST_ARCH" == "x86_64" || "$HOST_ARCH" == "amd64" ]]; then
        CXX_FLAGS="-std=c++20 -O2 -Wall -Wextra -static -march=x86-64-v2"
    else
        CXX_FLAGS="-std=c++20 -O2 -Wall -Wextra"
        echo "  ⚠ Non-x86 host ($HOST_ARCH); -march and -static skipped for local build"
    fi

    echo "  Compiler: $($COMPILER --version | head -1)"
    echo "  Flags: $CXX_FLAGS"

    # Try static linking, fall back to dynamic if static libc not available
    # shellcheck disable=SC2086
    if $COMPILER $CXX_FLAGS $SOURCES -o "$SUBMISSION_DIR/contestant.elf" 2>/dev/null; then
        echo "  ✓ Static build succeeded"
        STATIC="true"
    else
        echo "  ⚠ Static build failed, trying dynamic..."
        if [[ "$HOST_ARCH" == "x86_64" || "$HOST_ARCH" == "amd64" ]]; then
            CXX_FLAGS="-std=c++20 -O2 -Wall -Wextra -march=x86-64-v2"
        else
            CXX_FLAGS="-std=c++20 -O2 -Wall -Wextra"
        fi
        # shellcheck disable=SC2086
        $COMPILER $CXX_FLAGS $SOURCES -o "$SUBMISSION_DIR/contestant.elf" 2>&1
        STATIC="false"
    fi

    chmod +x "$SUBMISSION_DIR/contestant.elf"
    ELF_HASH=$(sha256sum "$SUBMISSION_DIR/contestant.elf" | awk '{print $1}')

    cat > "$SUBMISSION_DIR/run_metadata.json" <<EOF
{
    "compiler": "$COMPILER",
    "compiler_version": "$($COMPILER --version | head -1)",
    "cxx_flags": "$CXX_FLAGS",
    "static_linked": $STATIC,
    "elf_hash": "$ELF_HASH",
    "elf_size_bytes": $(stat -c%s "$SUBMISSION_DIR/contestant.elf" 2>/dev/null || stat -f%z "$SUBMISSION_DIR/contestant.elf"),
    "build_timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
    "build_mode": "local"
}
EOF
else
    echo "  Building in Docker (hermetic, --network none)..."
    docker build -t sandbox-builder "$SCRIPT_DIR/builder/" 2>&1 | tail -1
    docker run --rm --network none \
        -v "$SUBMISSION_DIR/src:/src:ro" \
        -v "$SUBMISSION_DIR:/output" \
        sandbox-builder 2>&1
fi

echo "  ELF: $SUBMISSION_DIR/contestant.elf"
echo ""

# ── Stage 3: ATTEST ─────────────────────────────────────────
echo "▸ Stage 3: ATTEST"
"$SCRIPT_DIR/attester/attest.sh" \
    "$SUBMISSION_DIR/contestant.elf" \
    --output "$SUBMISSION_DIR/attestation.json" 2>&1
echo ""

# ── Stage 4: PACK ───────────────────────────────────────────
if [[ "$SKIP_PACK" == "true" ]]; then
    echo "▸ Stage 4: PACK (skipped — requires root for ext4 mount)"
else
    echo "▸ Stage 4: PACK"
    "$SCRIPT_DIR/packer/pack_rootfs.sh" \
        "$SUBMISSION_DIR/contestant.elf" \
        --output "$SUBMISSION_DIR/rootfs.ext4" 2>&1
fi

echo ""
echo "═══════════════════════════════════════════════"
echo "  Pipeline complete"
echo "  Submission ID: $SUBMISSION_ID"
echo "  Output dir:    $SUBMISSION_DIR"
echo "═══════════════════════════════════════════════"
echo ""

# Print submission_id to stdout (for scripting)
echo "$SUBMISSION_ID"
