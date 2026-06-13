# Malicious-submission fixtures — and an honest status of the sandbox boundary

Four contestant submissions that each attempt something hostile. They exist to
test the platform's **defence in depth**, and — just as important — to keep us
honest about which layers are actually enforced today.

## What each fixture attempts

| Fixture | Attempts | Caught by intake static scan (Layer 1)? |
|---|---|---|
| `forkbomb.cpp` | `fork()` loop | **Yes — REJECTED** (`fork` is on the banned list) |
| `exfil_connect.cpp` | `socket()`+`connect()` outbound | No — `socket` is not banned; passes the scan |
| `read_shadow.cpp` | `open("/etc/shadow")` | No — `open` is not banned; passes the scan |
| `syscall_evasion.cpp` | `syscall(41,…)` = `socket` by number | No — no banned token to grep; passes the scan |

**Layer 1 is verified** (locally and in CI): running `intake/intake.sh` on each
fixture reproduces this column. The fork bomb dies at intake; the other three
sail past it — which is the whole point: `intake/intake.sh` says so itself
("source-scan is a lint heuristic only — defeated by syscall(2) by number,
inline asm, dlopen. Runtime seccomp is the enforced boundary").

## ⚠️ Enforcement gap — the runtime boundary is NOT wired yet

Layer 2 is the Firecracker microVM: a seccomp-bpf allowlist (`defaultAction:
KILL`), no-new-privs, read-only rootfs, and no network egress.

| Mechanism | Status in the repo |
|---|---|
| Read-only rootfs | ✅ real — `run_vm.sh` sets `is_read_only: true` |
| Hardware/VM isolation | ✅ real — Firecracker on KVM |
| **seccomp allowlist** | ✅ **enforced (code-complete, pending KVM validation)** — `sandbox/packer/init/seccomp_init.c` is a static PID-1 that sets `PR_SET_NO_NEW_PRIVS` and installs the BPF allowlist (default `KILL_PROCESS`) before `execve`. `pack_rootfs.sh` compiles it to `/init`; `run_vm.sh` boots `init=/init`. The `/etc/seccomp_profile.json` is now the human-readable spec of what the binary enforces. |
| Working guest userspace | ✅ real — the static `/init` needs no shell/busybox; it's the only binary the rootfs requires besides the contestant. |
| Network egress blocked | belt **and** suspenders — `connect` is omitted from the allowlist (seccomp KILLs outbound dial), and egress should also be blocked at the host (confirm `setup_network.sh` adds no NAT/MASQUERADE for the TAP). |

So `exfil_connect` (`connect` → not allowed → SIGSYS), `syscall_evasion` (raw
`socket`-by-number still matched by syscall *number* → SIGSYS), and `read_shadow`
(`/etc/shadow` absent in a read-only rootfs) are all stopped — and the design
note in `seccomp_init.c` explains exactly why each dies. **This is now built, not
asserted; the remaining step is to run it on KVM and record the kill.**

## Validation (on a `/dev/kvm` host) — see `sandbox/orchestrator/FIRECRACKER_RUNBOOK.md`

1. **Good submission boots + serves** — `pipeline.sh` (intake→build→attest→pack
   with the new `/init`) → `run_vm.sh` → the engine accepts the bot and scores.
   If a *legitimate* syscall is missing from the allowlist, rebuild `/init` with
   `-DSECCOMP_PERMISSIVE` (logs the missing syscall number on `ttyS0`), add it to
   `seccomp_init.c`, and rebuild — then ship the strict (KILL) build.
2. **Malicious submissions are killed** — `forkbomb` rejected at intake; the
   other three boot then die by `SIGSYS` (capture the kernel/`dmesg` line + the
   non-zero exit). `asciinema` + mp4 → `verified_runs/`.

Once the recorded KVM run exists, the truthful claim everywhere becomes
**"Firecracker microVM + read-only rootfs + enforced seccomp allowlist (kill on
disallowed syscall)"** — with the recording as evidence.
