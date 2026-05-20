#!/bin/bash
# run_benchmark.sh — Phase 4: Full benchmarking test
#
# Runs the end-to-end benchmark against a sandboxed contestant microVM.
# 1. Start the microVM via run_vm.sh
# 2. Wait for TCP port 9000 to become available
# 3. Pin bot to a specific CPU and run it against the VM IP
# 4. Tear down the VM
#
# Usage:
#   ./run_benchmark.sh --rootfs <path.ext4> [--duration <sec>]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOTFS=""
DURATION=5
TAP_DEV="fc-tap0"
VM_IP="172.16.0.2"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rootfs)   ROOTFS="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        *)          echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "$ROOTFS" ]]; then
    echo "Usage: $0 --rootfs <path.ext4> [--duration <sec>]" >&2
    exit 1
fi

if ! ip link show "$TAP_DEV" >/dev/null 2>&1; then
    echo "ERROR: Network not set up. Please run 'sudo ./setup_network.sh' first." >&2
    exit 1
fi

echo "═══════════════════════════════════════════════"
echo "  Phase 4: Sandboxed MicroVM Benchmark"
echo "═══════════════════════════════════════════════"

echo "[Benchmark] Starting microVM orchestrator..."
"$SCRIPT_DIR/run_vm.sh" --rootfs "$ROOTFS" --tap "$TAP_DEV" &
VM_SCRIPT_PID=$!

cleanup() {
    echo "[Benchmark] Tearing down..."
    kill -9 "$VM_SCRIPT_PID" 2>/dev/null || true
    wait "$VM_SCRIPT_PID" 2>/dev/null || true
}
trap cleanup EXIT

echo "[Benchmark] Waiting for contestant engine to bind TCP $VM_IP:9000..."
MAX_RETRIES=100
for ((i=1; i<=MAX_RETRIES; i++)); do
    # Try connecting to the port. If successful, it exits 0.
    if nc -z -w 1 "$VM_IP" 9000 2>/dev/null; then
        echo "[Benchmark] ✓ Contestant engine is reachable!"
        break
    fi
    sleep 0.1
    if [[ $i -eq $MAX_RETRIES ]]; then
        echo "ERROR: Timeout waiting for contestant engine on port 9000." >&2
        exit 1
    fi
done

echo "[Benchmark] Running bot..."
# Use taskset to pin the bot to CPU 0 (as an example of CPU isolation)
# The microVM can be pinned to a different core if needed in the future.
BOT_BIN="$SCRIPT_DIR/../../bot-engine/build/bot"
if [[ ! -x "$BOT_BIN" ]]; then
    echo "ERROR: Bot binary not found at $BOT_BIN" >&2
    exit 1
fi

# Run the bot
taskset -c 0 "$BOT_BIN" --ip "$VM_IP" --port 9000 --interval-us 1000 --duration-sec "$DURATION"

echo "[Benchmark] ✓ Run complete."
