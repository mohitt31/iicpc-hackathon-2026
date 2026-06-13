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

The intended Layer 2 is the Firecracker microVM: a seccomp-bpf allowlist
(`defaultAction: SCMP_ACT_KILL`), no-new-privs, read-only rootfs, and no network
egress. **As of this commit, most of Layer 2 is design intent, not enforced code:**

| Mechanism | Status in the repo |
|---|---|
| Read-only rootfs | ✅ real — `run_vm.sh` sets `is_read_only: true` |
| Hardware/VM isolation | ✅ real *in principle* — Firecracker on KVM — but see "boots?" below |
| **seccomp allowlist** | ❌ **decorative** — `pack_rootfs.sh` writes `/etc/seccomp_profile.json`, but nothing installs it (`grep -r seccomp_load\|prctl\|libseccomp` → nothing). The contestant runs unfiltered. |
| Working guest userspace | ❌ the rootfs has no shell/busybox and `run_vm.sh` sets no `init=`; the `#!/bin/sh` `/bin/init` has no interpreter to run it. The VM likely does not boot the contestant as-is. |
| Network egress blocked | ❓ unverified — `run_vm.sh` configures an `eth0` with an IP+gateway, not network-none. Depends on `setup_network.sh` not providing a route. |

So today, `exfil_connect` / `read_shadow` / `syscall_evasion` would **not** be
"killed by seccomp" — there is no seccomp to kill them. Claiming otherwise would
be exactly the kind of unmeasured assertion this platform is built to avoid.

## What Phase 1.4 must actually build (then record), on a `/dev/kvm` host

1. **A real guest init that installs the filter.** Replace the shell-script
   `/bin/init` with a static binary (a tiny C init, or busybox + a libseccomp
   loader) that: mounts `/proc /sys /dev`, applies the allowlist from
   `/etc/seccomp_profile.json` via `prctl(PR_SET_NO_NEW_PRIVS)` +
   `seccomp(SECCOMP_SET_MODE_FILTER, …)`, then `exec`s `/bin/contestant`.
   Set `init=/bin/init` (or `/sbin/init`) in `run_vm.sh`'s `boot_args`.
2. **Confirm no egress** in `setup_network.sh` (no NAT/MASQUERADE for the TAP),
   so `exfil_connect` fails at the network layer even independent of seccomp.
3. **Record the proof** with `asciinema`: boot a *good* submission (runs, scored),
   then each malicious fixture (forkbomb → rejected at intake; the other three →
   killed in-VM, with the `dmesg`/exit-code evidence of the seccomp `SIGSYS`).
   Save the cast + mp4 to `verified_runs/`.

Until step 1 lands, the truthful statement everywhere (UI, ARCHITECTURE, demo) is
**"Firecracker microVM + read-only rootfs; seccomp allowlist specified, runtime
enforcement in progress"** — not "submissions are seccomp-sandboxed."
