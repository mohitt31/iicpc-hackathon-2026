#!/usr/bin/env bash
# Phase 2.3 evidence capture. Run from the Mac with KUBECONFIG set to the
# Tailscale-reachable cluster:  ./2.3_run.sh | tee 2.3_distributed_run.txt
set -uo pipefail
NS=hft-bench
# Driver: defaults to `kubectl` (Mac/driver); on Aftab's host run with
#   KUBECTL="sudo k3s kubectl" bash 2.3_run.sh
kubectl() { ${KUBECTL:-kubectl} "$@"; }
line() { printf '\n========== %s ==========\n' "$1"; }

line "RUN META"
date -u +"%Y-%m-%dT%H:%M:%SZ  (UTC)"
kubectl version --short 2>/dev/null | sed 's/^/  /'

line "1. NODES (expect >= 3 Ready)"
kubectl get nodes -o wide
echo "node count: $(kubectl get nodes --no-headers | grep -c ' Ready ')"

line "2. NODE LABELS (benchmark selector)"
kubectl get nodes -L node-role.hft/benchmark

line "3. PODS PER NODE (one bot-fleet pod per node + engine/gateway/board)"
kubectl -n $NS get pods -o wide

line "4. BOT-FLEET ROLLOUT"
kubectl -n $NS get daemonset bot-fleet -o wide
echo "bots-per-pod arg:"; kubectl -n $NS get ds bot-fleet -o jsonpath='{.spec.template.spec.containers[0].args}'; echo

line "5. PER-POD ACCOUNTING (Sent / Acked / EAGAIN / Collisions)"
for p in $(kubectl -n $NS get pods -l app=bot-fleet -o name); do
  echo "--- $p (node: $(kubectl -n $NS get $p -o jsonpath='{.spec.nodeName}')) ---"
  kubectl -n $NS logs "$p" --tail=40 | grep -Ei 'sent|acked|eagain|collision|pool|connected' || echo "  (no accounting line yet — let it run longer)"
done

line "6. AGGREGATE CHECK"
echo "bot pods running: $(kubectl -n $NS get pods -l app=bot-fleet --field-selector=status.phase=Running --no-headers | wc -l | tr -d ' ')"
echo "Manual read: sum the per-pod Sent==Acked above. Headline = (#bot pods) x (bots-per-pod)."

line "DONE — commit this file to verified_runs/aftab/2.3_distributed_run.txt"
