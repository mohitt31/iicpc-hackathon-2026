# Infrastructure as Code

Two deployment paths, by purpose.

## `docker-compose.yml` — local / demo (runs today)

One command brings up the whole loop on a single machine:

```bash
docker compose -f infra/docker-compose.yml up --build
open http://localhost:8088          # leaderboard
# live feed: ws://localhost:8080/leaderboard/deltas
```

Services: `reference-engine`, `bot-fleet`, `telemetry-gateway`, `leaderboard`.

This path is for **correctness and demo**, not for the latency numbers —
containers add scheduler and network-namespace jitter that pollute the tail.
The bot's Linux-only perf features (CPU pinning, `SO_BUSY_POLL`) no-op here and
the bot degrades gracefully.

## `k8s/` + `terraform/` — production (real numbers)

The low-latency path. The bot fleet runs as a bare-metal `DaemonSet` with
Guaranteed QoS and exclusive pinned cores; benchmark nodes are provisioned by
Terraform with `isolcpus` / `nohz_full` kernel args and a label+taint so only
benchmark workloads land on them. This is where CPU pinning, `SO_BUSY_POLL`, and
HW NIC timestamping actually engage.

```bash
kubectl apply -f infra/k8s/platform.yaml
# terraform: fill in your provider + node-group resource, then `terraform apply`
```

## Status / honesty

- Compose: file structure, build contexts, ports, healthchecks verified by
  inspection; the telemetry gateway is exercised by an end-to-end WS client test.
  The full stack was **not** run in CI here (no Docker-in-sandbox).
- K8s/Terraform: deliverable blueprints mirroring Interface Contract §10. Image
  refs (`ghcr.io/REPLACE_ORG/...`) and the Terraform provider/node-group block
  are placeholders to wire to your registry and cloud. Not applied to a live
  cluster in this repo.
