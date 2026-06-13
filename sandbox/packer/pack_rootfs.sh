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

# ── Compile the seccomp-ENFORCING init (the real security boundary) ─────────
# Replaces the old decorative shell-script init (the minimal rootfs has no
# shell, so that never ran, and nothing installed the seccomp filter). This
# static binary becomes PID 1: it mounts /proc,/sys,/dev, sets no-new-privs,
# installs the seccomp-bpf allowlist (default = KILL), then exec's the
# contestant — which inherits the filter. Source + allowlist rationale:
# sandbox/packer/init/seccomp_init.c. Booted via init=/init (see run_vm.sh).
INIT_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/init/seccomp_init.c"
if [[ ! -f "$INIT_SRC" ]]; then
    echo "PACK FAILED: missing seccomp init source: $INIT_SRC" >&2
    exit 1
fi
echo "[Packer] Compiling static seccomp init (the enforced boundary)..."
# -static so the rootfs needs no shared libs. Add -DSECCOMP_PERMISSIVE for
# bring-up (ENOSYS + logs missing syscalls instead of KILL) — see the .c header.
if ! gcc -O2 -static -Wall -o "$MOUNT_DIR/init" "$INIT_SRC" 2>/tmp/seccomp_init_build.log; then
    echo "PACK FAILED: could not build seccomp init:" >&2
    cat /tmp/seccomp_init_build.log >&2
    exit 1
fi
chmod +x "$MOUNT_DIR/init"

# ── Seccomp profile: human-readable spec of what /init ENFORCES ─────────────
# (Documentation only — the authority is the BPF filter compiled into /init.
# Keep this in sync with sandbox/packer/init/seccomp_init.c.)
cat > "$MOUNT_DIR/etc/seccomp_profile.json" <<'SECCOMP_EOF'
{
    "comment": "ENFORCED by /init (seccomp_init.c). Allowlist for a TCP epoll matching engine; default action KILL. `connect`, fork/vfork, ptrace, mount, chroot, pivot_root, kexec, reboot, *module, setuid/setgid, bpf are NOT allowed and are killed with SIGSYS.",
    "defaultAction": "SCMP_ACT_KILL_PROCESS",
    "architectures": ["SCMP_ARCH_X86_64"],
    "syscalls": [
        {"names": ["read","write","readv","writev","close","lseek","pread64","pwrite64",
                   "socket","bind","listen","accept","accept4","setsockopt","getsockopt",
                   "getsockname","getpeername","shutdown","recvfrom","sendto","recvmsg","sendmsg",
                   "epoll_create1","epoll_ctl","epoll_wait","epoll_pwait","poll","ppoll","pselect6",
                   "eventfd2","timerfd_create","timerfd_settime",
                   "mmap","munmap","mprotect","brk","madvise","mremap",
                   "clone","clone3","futex","set_robust_list","get_robust_list","rseq",
                   "sched_yield","sched_getaffinity","sched_setaffinity",
                   "rt_sigaction","rt_sigprocmask","rt_sigreturn","sigaltstack",
                   "fcntl","dup","dup2","dup3","pipe2","openat","fstat","newfstatat","statx",
                   "getdents64","uname","statfs",
                   "clock_gettime","clock_getres","clock_nanosleep","nanosleep","gettimeofday",
                   "arch_prctl","set_tid_address","gettid","getpid","getppid","getrandom",
                   "prlimit64","getuid","geteuid","getgid","getegid","exit","exit_group","execve"],
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
echo "[Packer]   Contents: /bin/contestant, /init (seccomp-enforcing), /etc/seccomp_profile.json"
echo "[Packer]   Boundary: /init installs a seccomp-bpf allowlist (default KILL) before exec — boot with init=/init"
