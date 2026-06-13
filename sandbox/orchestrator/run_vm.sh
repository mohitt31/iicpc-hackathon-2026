#!/bin/bash
# run_vm.sh — Phase 3: Firecracker MicroVM Orchestrator
#
# Configures and boots a Firecracker microVM using the provided kernel and rootfs.
# Configures a network interface linked to the host's TAP device.
#
# Usage:
#   ./run_vm.sh --rootfs <path.ext4> --tap <tap_name> [--kernel <vmlinux>] [--mac <mac_addr>]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FC_BIN="$SCRIPT_DIR/bin/firecracker"
DEFAULT_KERNEL="$SCRIPT_DIR/bin/vmlinux"

ROOTFS=""
TAP_DEV=""
KERNEL="$DEFAULT_KERNEL"
MAC="AA:FC:00:00:00:01"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rootfs) ROOTFS="$2"; shift 2 ;;
        --tap)    TAP_DEV="$2"; shift 2 ;;
        --kernel) KERNEL="$2"; shift 2 ;;
        --mac)    MAC="$2"; shift 2 ;;
        *)        echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "$ROOTFS" ]] || [[ -z "$TAP_DEV" ]]; then
    echo "Usage: $0 --rootfs <ext4> --tap <tap_dev>" >&2
    exit 1
fi

if [[ ! -x "$FC_BIN" ]]; then
    echo "ERROR: Firecracker binary not found at $FC_BIN. Run setup_deps.sh first." >&2
    exit 1
fi

API_SOCKET=$(mktemp -u -t fc-api-socket.XXXXXX)
LOG_FILE=$(mktemp -t fc-log.XXXXXX)

echo "[Orchestrator] Starting Firecracker microVM..."
echo "  Kernel: $KERNEL"
echo "  Rootfs: $ROOTFS"
echo "  TAP:    $TAP_DEV ($MAC)"
echo "  API Socket: $API_SOCKET"

# Start Firecracker in the background
rm -f "$API_SOCKET"
"$FC_BIN" --api-sock "$API_SOCKET" > "$LOG_FILE" 2>&1 &
FC_PID=$!

cleanup() {
    kill "$FC_PID" 2>/dev/null || true
    wait "$FC_PID" 2>/dev/null || true
    rm -f "$API_SOCKET"
}
trap cleanup EXIT

# Wait for API socket to be ready
while [[ ! -e "$API_SOCKET" ]]; do
    sleep 0.01
done

# Helper to send API requests
api_put() {
    local path="$1"
    local data="$2"
    local status
    status=$(curl -s -w "%{http_code}" -X PUT --unix-socket "$API_SOCKET" \
        -H "Accept: application/json" -H "Content-Type: application/json" \
        -d "$data" "http://localhost$path" -o /dev/null)
    
    if [[ "$status" != "204" ]]; then
        echo "API PUT $path failed with status $status" >&2
        cat "$LOG_FILE" >&2
        exit 1
    fi
}

# 1. Configure Boot Source (Kernel)
api_put "/boot-source" "{
    \"kernel_image_path\": \"$KERNEL\",
    \"boot_args\": \"console=ttyS0 reboot=k panic=1 pci=off init=/init ip=172.16.0.2::172.16.0.1:255.255.255.0::eth0:off\"
}"

# 2. Configure Rootfs Drive
api_put "/drives/rootfs" "{
    \"drive_id\": \"rootfs\",
    \"path_on_host\": \"$ROOTFS\",
    \"is_root_device\": true,
    \"is_read_only\": true
}"

# 3. Configure Network Interface
api_put "/network-interfaces/eth0" "{
    \"iface_id\": \"eth0\",
    \"guest_mac\": \"$MAC\",
    \"host_dev_name\": \"$TAP_DEV\"
}"

# 4. Start the instance
api_put "/actions" "{
    \"action_type\": \"InstanceStart\"
}"

echo "[Orchestrator] MicroVM is running (PID: $FC_PID). Press Ctrl+C to stop."
wait "$FC_PID"
