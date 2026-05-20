#!/bin/bash
# setup_deps.sh — Download Firecracker and Linux kernel for Phase 3
#
# This script downloads the required binaries to run Firecracker MicroVMs.
# 1. Firecracker binary (v1.7.0)
# 2. Sample Linux kernel (vmlinux)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN_DIR="$SCRIPT_DIR/bin"
mkdir -p "$BIN_DIR"

echo "[Setup] Downloading dependencies to $BIN_DIR"

# ── Download Firecracker ──────────────────────────────────────
FC_VERSION="v1.7.0"
FC_TAR="firecracker-${FC_VERSION}-x86_64.tgz"

if [[ ! -f "$BIN_DIR/firecracker" ]]; then
    echo "[Setup] Downloading Firecracker ${FC_VERSION}..."
    curl -L "https://github.com/firecracker-microvm/firecracker/releases/download/${FC_VERSION}/${FC_TAR}" -o "$BIN_DIR/${FC_TAR}"
    
    tar -xzf "$BIN_DIR/${FC_TAR}" -C "$BIN_DIR"
    mv "$BIN_DIR/release-${FC_VERSION}-x86_64/firecracker-${FC_VERSION}-x86_64" "$BIN_DIR/firecracker"
    chmod +x "$BIN_DIR/firecracker"
    
    # Cleanup
    rm -rf "$BIN_DIR/release-${FC_VERSION}-x86_64" "$BIN_DIR/${FC_TAR}"
else
    echo "[Setup] Firecracker already downloaded."
fi

# ── Download Linux Kernel ─────────────────────────────────────
# We use a pre-built minimal kernel provided by the Firecracker team
KERNEL_URL="https://s3.amazonaws.com/spec.ccfc.min/img/quickstart_guide/x86_64/kernels/vmlinux.bin"

if [[ ! -f "$BIN_DIR/vmlinux" ]]; then
    echo "[Setup] Downloading Linux kernel..."
    curl -L "$KERNEL_URL" -o "$BIN_DIR/vmlinux"
else
    echo "[Setup] Linux kernel already downloaded."
fi

echo "[Setup] ✓ Dependencies ready."
