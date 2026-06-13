# Bare-Metal Production Measurement — Test Plan

> **STATUS: DONE (PR #40).** The bare-metal run was executed on an isolated i7-13620H
> (`verified_runs/aftab/baremetal_latency.txt`): measured p99 = 7.7µs, clean-run
> naive≈CO confirmed. RESULTS.md now carries the measured block. This file is retained
> as the reproduction procedure.

Goal: replace the "design target" label in RESULTS.md with a measured, reproducible
bare-metal p99, plus the honesty checks (clean-run naive≈CO; planted-stall CO gap).

## 0. Instance & topology
- **Instance:** AWS `c6i.metal` (no hypervisor → real BIOS/CPU control). `c6in.metal`
  if NIC headroom matters.
- **Phase A (engine latency, do first):** engine + bot on ONE metal box over
  loopback. Removes network variance; isolates the engine + kernel-stack latency.
- **Phase B (true wire latency):** two metal boxes in the SAME AZ + a **cluster
  placement group**; bot on box 1, engine on box 2.
- AMI: Amazon Linux 2023 or Ubuntu 24.04 (kernel ≥ 4.5, ENA driver with
  CONFIG_NET_RX_BUSY_POLL). Note: our RTT is measured entirely on the bot clock
  (`now − intended_send`), so **no PTP/clock-sync is needed** for Phase A or B.

## 1. Kernel cmdline (GRUB), reboot to apply
```
isolcpus=1-15 nohz_full=1-15 rcu_nocbs=1-15 mitigations=off transparent_hugepage=never
# leave core 0 as housekeeping. (idle=poll optional: lower latency, higher power.)
```

## 2. Host tuning (run as root, once per boot)
```bash
echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
echo off > /sys/devices/system/cpu/smt/control          # disable hyperthreading
swapoff -a
echo 0 > /proc/sys/kernel/numa_balancing
# NIC busy-poll (ENA): AWS-recommended 50µs
sysctl -w net.core.busy_read=50 net.core.busy_poll=50
# move ENA IRQs OFF the isolated cores onto core 0
systemctl stop irqbalance
for i in $(grep -l . /proc/irq/*/smp_affinity_list 2>/dev/null); do echo 0 > "$i"; done
```

## 3. Build & run
```bash
cd bot-engine && mkdir -p build && cd build && cmake .. && make -j
# engine pinned to an isolated core:
taskset -c 1 ./reference_server --port 9000 &
# integrity gate MUST pass first (instrument must beat what it measures):
taskset -c 2-9 ./bot --bots 8 --start-core 2 --interval-us 50 --duration-sec 60 \
       --warmup-orders 50000 --snapshot-dir /tmp/tel
# (a) CLEAN run  -> expect naive p99 ≈ CO p99 (within ~1.1x): proves honesty
# (b) STALL run  -> CO proof:
./null_responder --stall-mode --stall-every 20000 --stall-ms 5 &
./bot --bots 1 --interval-us 100 --duration-sec 60 --no-gate
```

## 4. Acceptance criteria (all must hold to publish)
- Integrity gate PASSES (self-test p99 well under 5µs).
- CLEAN run: naive vs CO p99 agree within ~1.1x  (methodology not lying).
- Isolated-core sanity:  `perf stat -e sched:sched_switch -C 2-9 -- sleep 10   ≈ 0`
                          `perf stat -e irq_vectors:local_timer_entry -C 2-9    ≈ 0`
- Capture the full ladder p50→p99→p99.9→p99.99→max, 3 runs, report the median run.

## 5. Report & update docs
- Record an environment manifest: lscpu, uname -a, /proc/cmdline, ENA driver
  version (modinfo ena), the sysctls above.
- Replace the RESULTS.md "DESIGN TARGET" block with the measured p50/p99 and the
  clean-run agreement ratio. THAT becomes the defensible production number.

## 6. Cost / teardown
c6i.metal ≈ $5–6/hr on-demand; budget 1–2 hrs. `terraform destroy` after.

## Known gotcha
ENA NAPI busy-poll has a documented interrupt-unmask race on some driver versions
(amzn-drivers issue #312). Pin `modinfo ena` version, and verify busy-poll actually
engages (interrupt-driven % should drop sharply under load).
