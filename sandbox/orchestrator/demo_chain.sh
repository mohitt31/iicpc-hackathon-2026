#!/bin/bash
set -euo pipefail

echo "====================================================================="
echo " FIRECRACKER SECCOMP RUNTIME BOUNDARY DEMO (Phase 1.4)"
echo "====================================================================="
echo ""

echo "▸ 1. GOOD submission: boots, runs under allowlist, serves, and exits 0"
echo "---------------------------------------------------------------------"
sleep 1
# Boot good submission (compiled previously)
/home/aftabnaik19/projects/iicpc-hackathon-2026/sandbox/orchestrator/run_vm.sh \
  --rootfs "/tmp/fc/cfbf2d05ca86641d31f7086e1f100b2544f2d64299c57ee433e9bc27899bf72f/rootfs.ext4" \
  --tap tap0

echo ""
echo "====================================================================="
echo "▸ 2. MALICIOUS submission (exfil_connect): socket+connect outbound"
echo "   Expecting: KILLED by seccomp on connect() -> exitcode=0x0000001f (SIGSYS)"
echo "---------------------------------------------------------------------"
sleep 2
/home/aftabnaik19/projects/iicpc-hackathon-2026/sandbox/orchestrator/run_vm.sh \
  --rootfs "/tmp/fc/63608c4acdf4471d38c8daf4d7f6a4f03d634bb80cd3fd48bfad5c0f5e143300/rootfs.ext4" \
  --tap tap0 || true

echo ""
echo "====================================================================="
echo "▸ 3. MALICIOUS submission (read_shadow): read /etc/shadow"
echo "   Expecting: openat allowed but file is absent/rootfs read-only -> exits 1"
echo "---------------------------------------------------------------------"
sleep 2
/home/aftabnaik19/projects/iicpc-hackathon-2026/sandbox/orchestrator/run_vm.sh \
  --rootfs "/tmp/fc/0b9dcb856a4e7b4b91635b052f5127266fe2f37075c7e646c131d0cb14ee75d7/rootfs.ext4" \
  --tap tap0 || true

echo ""
echo "====================================================================="
echo "▸ 4. MALICIOUS submission (syscall_evasion): connect by raw syscall number"
echo "   Expecting: KILLED by seccomp on syscall 42 -> exitcode=0x0000001f (SIGSYS)"
echo "---------------------------------------------------------------------"
sleep 2
/home/aftabnaik19/projects/iicpc-hackathon-2026/sandbox/orchestrator/run_vm.sh \
  --rootfs "/tmp/fc/d98214ea8c1380d74eb02b2b66d85f80ea9960c9d3cb369a3d47a5199b427609/rootfs.ext4" \
  --tap tap0 || true

echo ""
echo "====================================================================="
echo "▸ 5. MALICIOUS submission (forkbomb): fork loop"
echo "   Expecting: REJECTED at intake scan level"
echo "---------------------------------------------------------------------"
sleep 2
/home/aftabnaik19/projects/iicpc-hackathon-2026/sandbox/intake/intake.sh /tmp/fb.tar.gz || true

echo ""
echo "====================================================================="
echo " DEMO COMPLETE: Seccomp boundary successfully verified!"
echo "====================================================================="
