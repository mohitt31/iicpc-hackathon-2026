#!/bin/bash
# setup_network.sh — Phase 4: Host Network Setup for MicroVM
#
# Creates a TAP interface on the host and assigns it an IP address
# so the bot can communicate with the contestant engine inside the microVM.
#
# Usage:
#   sudo ./setup_network.sh [tap_name] [ip_addr]
#

set -euo pipefail

TAP_DEV=${1:-fc-tap0}
IP_ADDR=${2:-172.16.0.1/24}

if [[ $EUID -ne 0 ]]; then
    echo "This script must be run as root (use sudo)" >&2
    exit 1
fi

echo "[Network] Setting up TAP device $TAP_DEV..."

# Check if TAP already exists
if ip link show "$TAP_DEV" >/dev/null 2>&1; then
    echo "[Network] $TAP_DEV already exists. Resetting..."
    ip link set "$TAP_DEV" down
    ip link delete "$TAP_DEV"
fi

# Create TAP interface assigned to the real user who ran sudo
REAL_USER=${SUDO_USER:-$(whoami)}

ip tuntap add dev "$TAP_DEV" mode tap user "$REAL_USER"
ip addr add "$IP_ADDR" dev "$TAP_DEV"
ip link set "$TAP_DEV" up

echo "[Network] ✓ $TAP_DEV created and assigned $IP_ADDR"
