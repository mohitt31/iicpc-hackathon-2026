#!/usr/bin/env bash
# ============================================================================
#  run_distributed_scale.sh — turnkey 3-node distributed scale run (Phase 2.3)
# ============================================================================
#  PURPOSE: plug-and-play the hardware session with Aftab. Labels the nodes,
#  applies the k8s manifests, scales the bot fleet across >=3 machines, and
#  captures the evidence (kubectl get pods -o wide + the fleet accounting) into
#  a committable run log.
#
#  HONESTY DISCIPLINE (non-negotiable — this is the whole brand):
#   - This run proves SCALE + ACCOUNTING across real nodes, NOT latency.
#     (Per-bot latency comes from the isolated-core bare-metal run, 7.7us.)
#   - Target is ~30,000 concurrent bots across the nodes, but you COMMIT THE
#     LARGEST COUNT WHERE THE ACCOUNTING CLOSES (Sent within tolerance of Acked,
#     0 collisions, 0 double-counts). If 30k starves and 12k is clean, commit
#     12k. A clean smaller number beats a dirty big one.
#   - At high bot counts the per-bot interval MUST be widened so aggregate
#     orders/sec stays in the band the engine sustains. A starved connection
#     that never transacts is not a bot. Label the result "N concurrent
#     participants at a realistic per-client cadence", never "N at line rate".
#
#  PREREQUISITES (the hardware session, not this script's job):
#   - >=3 Linux machines reachable over a network (laptops + Tailscale is fine).
#   - k3s installed: one server node + >=2 agents joined to the cluster.
#   - The iicpc-bot-engine image pulled/available to each node
#     (ghcr.io/mohitt31/iicpc-bot-engine:latest), or k3d with --registry.
#   - kubectl context pointing at the cluster (kubectl get nodes shows >=3 Ready).
#
#  USAGE:
#     ./run_distributed_scale.sh                       # defaults: ~30k target
#     BOTS_PER_NODE=4000 INTERVAL_US=500 ./run_distributed_scale.sh
#     DRY_RUN=1 ./run_distributed_scale.sh             # print actions, apply nothing
# ============================================================================
set -euo pipefail

# ---- tunables (override via env) -------------------------------------------
BOTS_PER_NODE="${BOTS_PER_NODE:-10000}"   # 3 nodes x 10k = 30k target
INTERVAL_US="${INTERVAL_US:-500}"         # widened cadence — sustainable aggregate
DURATION_SEC="${DURATION_SEC:-30}"
MANIFEST="${MANIFEST:-infra/k8s/platform.yaml}"
NAMESPACE="${NAMESPACE:-iicpc}"
OUT_DIR="${OUT_DIR:-verified_runs/distributed}"
DRY_RUN="${DRY_RUN:-0}"

run() { echo "+ $*"; [ "$DRY_RUN" = "1" ] || "$@"; }

echo "=== IICPC distributed scale run (Phase 2.3) ==="
echo "    target: ${BOTS_PER_NODE} bots/node x (nodes) | interval ${INTERVAL_US}us | ${DURATION_SEC}s"
echo "    NOTE: this is a SCALE + ACCOUNTING run, not a latency run."
echo

# ---- 0. preflight: need >=3 Ready nodes ------------------------------------
READY=$(kubectl get nodes --no-headers 2>/dev/null | grep -cw Ready || true); READY=$(printf '%s' "${READY:-0}" | tr -dc '0-9'); READY=${READY:-0}
echo "Ready nodes: ${READY}"
if [ "${READY}" -lt 3 ]; then
  echo "!! Need >=3 Ready nodes for a genuine distributed run. Found ${READY}."
  echo "   (Single-node only de-risks the deploy path — that's #42, already done."
  echo "    Do NOT commit a single-node result as the distributed run.)"
  [ "$DRY_RUN" = "1" ] || exit 1
fi

# ---- 1. label the nodes so the DaemonSet/scheduler can target them ---------
echo "--- labelling nodes iicpc-role=benchmark ---"
for n in $(kubectl get nodes --no-headers -o custom-columns=NAME:.metadata.name); do
  run kubectl label node "$n" iicpc-role=benchmark --overwrite
done

# ---- 2. apply the platform manifests ---------------------------------------
echo "--- applying ${MANIFEST} (namespace ${NAMESPACE}) ---"
run kubectl create namespace "${NAMESPACE}" --dry-run=client -o yaml | { [ "$DRY_RUN" = "1" ] && cat || kubectl apply -f -; }
run kubectl apply -n "${NAMESPACE}" -f "${MANIFEST}"

# ---- 3. scale the bot fleet: set per-node bot count + widened interval ------
#  The bot-fleet is a DaemonSet (one pod per labelled node); we patch its args
#  so each pod launches BOTS_PER_NODE bots at the sustainable INTERVAL_US.
echo "--- patching bot-fleet args: --bots=${BOTS_PER_NODE} --interval-us=${INTERVAL_US} ---"
PATCH=$(cat <<JSON
{"spec":{"template":{"spec":{"containers":[{"name":"bot",
 "args":["--bots=${BOTS_PER_NODE}","--interval-us=${INTERVAL_US}","--duration-sec=${DURATION_SEC}","--no-gate","--snapshot-dir=/snap"]}]}}}}
JSON
)
run kubectl patch daemonset bot-fleet -n "${NAMESPACE}" --type strategic -p "${PATCH}"

# ---- 4. wait for rollout, then capture the evidence ------------------------
echo "--- waiting for bot-fleet rollout across nodes ---"
run kubectl rollout status daemonset/bot-fleet -n "${NAMESPACE}" --timeout=120s || true

mkdir -p "${OUT_DIR}"
STAMP=$(date +%Y%m%d_%H%M%S)
LOG="${OUT_DIR}/distributed_scale_${STAMP}.txt"

echo "--- capturing kubectl get pods -o wide (proves pods are spread across nodes) ---"
{
  echo "# IICPC distributed scale run — ${STAMP}"
  echo "# target ${BOTS_PER_NODE} bots/node, interval ${INTERVAL_US}us, ${DURATION_SEC}s"
  echo "# THIS PROVES: scale + accounting across nodes. NOT latency."
  echo
  echo "## nodes"
  kubectl get nodes -o wide
  echo
  echo "## pods (note the NODE column — must show >=3 distinct nodes)"
  kubectl get pods -n "${NAMESPACE}" -o wide
  echo
} | tee "${LOG}"

# ---- 5. let the run finish, then pull each pod's accounting -----------------
echo "--- letting the ${DURATION_SEC}s run complete, then collecting accounting ---"
[ "$DRY_RUN" = "1" ] || sleep $((DURATION_SEC + 10))

{
  echo "## per-pod accounting (Sent / Acked / aborts / collisions)"
  for p in $(kubectl get pods -n "${NAMESPACE}" -l app=bot-fleet --no-headers -o custom-columns=NAME:.metadata.name 2>/dev/null); do
    echo "### ${p}"
    kubectl logs -n "${NAMESPACE}" "${p}" 2>/dev/null | grep -iE "Sent=|Acked=|Connected:|PartialAbort|Collision|PoolExhaust|EAGAIN" | tail -5
    echo
  done
} | tee -a "${LOG}"

echo
echo "=== DONE. Evidence written to ${LOG} ==="
echo
echo "BEFORE committing this as the distributed-scale result, verify:"
echo "  1. The pods column shows >=3 DISTINCT nodes (genuinely distributed)."
echo "  2. Aggregate Sent is within tolerance of aggregate Acked (accounting closes)."
echo "  3. 0 collisions, 0 double-counts."
echo "  4. Commit the LARGEST bot count where 1-3 hold — if this count starved,"
echo "     re-run with a smaller BOTS_PER_NODE and/or larger INTERVAL_US."
echo "  5. Update verified_runs/canonical.json + RESULTS.md with the committed"
echo "     count, framed as 'scale + accounting under backpressure', NOT 'Sent==Acked',"
echo "     and NOT as a latency result."
echo
echo "Then the multi-node 'pending' item in canonical.json open_items can be closed."
