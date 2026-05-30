#!/bin/bash
# pack_rootfs.sh — Rootfs packer for Firecracker microVM (§7 stage 4)
#
# Takes a contestant ELF and creates a minimal ext4 filesystem image
# suitable for Firecracker's --root-drive.
#
# Per doc §7 stage 4:
#   ELF + minimal rootfs → Firecracker block device image
#   seccomp-bpf allowlist, no-new-privs, read-only rootfs
#
# Usage:
#   ./pack_rootfs.sh <contestant.elf> [--output <output.ext4>] [--size-mb <MB>]
#
# Requirements:
#   - None (runs unprivileged using mkfs.ext4 -d)
#
# Output:
#   - <output.ext4> — mountable ext4 image with /contestant binary

set -euo pipefail

ELF_PATH=""
OUTPUT_FILE=""
SIZE_MB=64  # 64MB default — enough for a static binary + minimal rootfs

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output)  OUTPUT_FILE="$2"; shift 2 ;;
        --size-mb) SIZE_MB="$2";     shift 2 ;;
        -*)        echo "ERROR: Unknown option: $1" >&2; exit 1 ;;
        *)         ELF_PATH="$1";     shift ;;
    esac
done

if [[ -z "$ELF_PATH" ]]; then
    echo "ERROR: No ELF path provided." >&2
    echo "Usage: $0 <contestant.elf> [--output <output.ext4>] [--size-mb <MB>]" >&2
    exit 1
fi

if [[ ! -f "$ELF_PATH" ]]; then
    echo "PACK FAILED: ELF not found: $ELF_PATH" >&2
    exit 1
fi

if [[ -z "$OUTPUT_FILE" ]]; then
    OUTPUT_FILE="$(dirname "$ELF_PATH")/rootfs.ext4"
fi

echo "[Packer] Creating ${SIZE_MB}MB ext4 image..."

# ── Create minimal directory structure ──────────────────────
MOUNT_DIR=$(mktemp -d)
cleanup() {
    rm -rf "$MOUNT_DIR" 2>/dev/null || true
}
trap cleanup EXIT

mkdir -p "$MOUNT_DIR"/{bin,dev,etc,proc,sys,tmp,run}

# Copy contestant binary
cp "$ELF_PATH" "$MOUNT_DIR/bin/contestant"
chmod +x "$MOUNT_DIR/bin/contestant"

# Create minimal /etc files
echo "root:x:0:0:root:/:/bin/sh" > "$MOUNT_DIR/etc/passwd"
echo "root:x:0:" > "$MOUNT_DIR/etc/group"
echo "contestant-microvm" > "$MOUNT_DIR/etc/hostname"

# Create init script that runs the contestant
cat > "$MOUNT_DIR/bin/init" <<'INIT_EOF'
#!/bin/sh
# Minimal init for Firecracker microVM
# Mounts proc/sys, then exec's the contestant binary

mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs devtmpfs /dev

echo "[microvm] Starting contestant engine..."
exec /bin/contestant "$@"
INIT_EOF
chmod +x "$MOUNT_DIR/bin/init"

# ── Generate seccomp profile reference ──────────────────────
cat > "$MOUNT_DIR/etc/seccomp_profile.json" <<'SECCOMP_EOF'
{
    "comment": "Seccomp-bpf allowlist for contestant microVM (§8)",
    "defaultAction": "SCMP_ACT_KILL",
    "architectures": ["SCMP_ARCH_X86_64"],
    "syscalls": [
        {"names": ["read", "write", "close", "fstat", "lseek",
                   "mmap", "mprotect", "munmap", "brk",
                   "exit", "exit_group",
                   "clock_gettime", "clock_getres",
                   "nanosleep", "sched_yield",
                   "futex", "set_robust_list",
                   "getrandom", "arch_prctl",
                   "set_tid_address", "gettid"],
         "action": "SCMP_ACT_ALLOW"}
    ]
}
SECCOMP_EOF

# ── Build ext4 image unprivileged ───────────────────────────
dd if=/dev/zero of="$OUTPUT_FILE" bs=1M count="$SIZE_MB" status=none
mkfs.ext4 -d "$MOUNT_DIR" -F -q "$OUTPUT_FILE"

ELF_SIZE=$(stat -c%s "$ELF_PATH" 2>/dev/null || stat -f%z "$ELF_PATH")
IMG_SIZE=$(stat -c%s "$OUTPUT_FILE" 2>/dev/null || stat -f%z "$OUTPUT_FILE")

echo "[Packer] ✓ Rootfs image created: $OUTPUT_FILE"
echo "[Packer]   ELF size: $ELF_SIZE bytes"
echo "[Packer]   Image size: $IMG_SIZE bytes (${SIZE_MB}MB)"
echo "[Packer]   Contents: /bin/contestant, /bin/init, /etc/seccomp_profile.json"
