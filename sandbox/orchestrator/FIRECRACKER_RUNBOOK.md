# Firecracker microVM — validation & recording runbook (Phase 1.4)

Run this on a `/dev/kvm` host. It proves the sandbox boundary is **real**: a good
submission boots and serves; a malicious one is **killed by the seccomp filter**
that `/init` now installs (`sandbox/packer/init/seccomp_init.c`). The recording is
the Phase-1.4 deliverable.

## 0. Prereqs (one-time)
```bash
ls -la /dev/kvm                      # must exist
which gcc asciinema || sudo pacman -S --noconfirm gcc asciinema
sudo bash sandbox/orchestrator/setup_deps.sh    # fetches firecracker + vmlinux into sandbox/orchestrator/bin/
sudo bash sandbox/orchestrator/setup_network.sh # creates the TAP; CONFIRM it does NOT add NAT/MASQUERADE (no egress)
```

## 1. Pack a GOOD submission and prove it boots + runs under seccomp
A "good" engine only uses allowed syscalls (socket/bind/listen/accept + read/write/epoll).
```bash
# minimal good engine: opens a listen socket then exits 0 — exercises the allowlist
mkdir -p /tmp/good/src && cat > /tmp/good/src/main.cpp <<'EOF'
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstdio>
int main(){ int s=socket(AF_INET,SOCK_STREAM,0); sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(9000);
            bind(s,(sockaddr*)&a,sizeof(a)); listen(s,16); std::puts("engine up"); return 0; }
EOF
( cd /tmp/good/src && COPYFILE_DISABLE=1 tar czf /tmp/good.tar.gz main.cpp )

# pipeline builds + attests + packs the rootfs WITH the new /init (compiles seccomp_init.c):
sudo bash sandbox/pipeline.sh /tmp/good.tar.gz --output-dir /tmp/fc
GOOD_DIR=$(ls -d /tmp/fc/*/ | head -1)

# boot it
sudo bash sandbox/orchestrator/run_vm.sh --rootfs "${GOOD_DIR%/}/rootfs.ext4" --tap tap0
# EXPECT on console: "[init] installing seccomp allowlist (default=KILL)"
#                    "[init] sandbox active -> exec /bin/contestant"
#                    "engine up"   ← contestant ran under the filter, NOT killed.
```
If the good engine dies with SIGSYS, a legitimate syscall is missing from the
allowlist: rebuild `/init` permissively to find it —
`gcc -O2 -static -DSECCOMP_PERMISSIVE -o /init sandbox/packer/init/seccomp_init.c`,
boot again, read the `errno/ENOSYS` syscall number off `ttyS0`, add it to
`seccomp_init.c`'s allowlist, rebuild strict, re-pack.

## 2. Prove MALICIOUS submissions are killed
```bash
for f in exfil_connect read_shadow syscall_evasion; do     # forkbomb is rejected at intake (layer 1)
  mkdir -p /tmp/$f/src && cp sandbox/test/malicious/$f.cpp /tmp/$f/src/main.cpp
  ( cd /tmp/$f/src && COPYFILE_DISABLE=1 tar czf /tmp/$f.tar.gz main.cpp )
  echo "=== $f ===" | tee -a verified_runs/firecracker_malicious.txt
  sudo bash sandbox/pipeline.sh /tmp/$f.tar.gz --output-dir /tmp/fc 2>&1 | tee -a verified_runs/firecracker_malicious.txt
  D=$(ls -dt /tmp/fc/*/ | head -1)
  sudo bash sandbox/orchestrator/run_vm.sh --rootfs "${D%/}/rootfs.ext4" --tap tap0 2>&1 | tee -a verified_runs/firecracker_malicious.txt
done
# also show forkbomb dies at intake:
mkdir -p /tmp/fb/src && cp sandbox/test/malicious/forkbomb.cpp /tmp/fb/src/main.cpp
( cd /tmp/fb/src && COPYFILE_DISABLE=1 tar czf /tmp/fb.tar.gz main.cpp )
sudo bash sandbox/intake/intake.sh /tmp/fb.tar.gz 2>&1 | tee -a verified_runs/firecracker_malicious.txt   # EXPECT: REJECTED (fork banned)
```
EXPECT: each of the three boots, then the kernel terminates it on the disallowed
syscall — a `SIGSYS` / `seccomp` line in the console output and a non-zero exit.
That line is the proof.

## 3. Record it (the deliverable)
```bash
asciinema rec verified_runs/firecracker_1.4.cast -c 'bash sandbox/orchestrator/demo_chain.sh'   # or re-run steps 1+2 live
# optional mp4: agg verified_runs/firecracker_1.4.cast verified_runs/firecracker_1.4.mp4
git add verified_runs/firecracker_* sandbox/
git commit -m "Phase 1.4: Firecracker microVM — good boots+serves, malicious killed by seccomp (recorded)"
git push
```

## What this proves for the judges
The microVM boundary is **enforced, not asserted**: the same `seccomp_init.c`
allowlist that lets a real matching engine serve TCP also kills outbound exfil
(`connect` not allowed), credential theft (`/etc/shadow` absent, rootfs read-only),
and the syscall-by-number evasion that defeats the intake lint. One recording,
four fixtures, the boundary holds.
