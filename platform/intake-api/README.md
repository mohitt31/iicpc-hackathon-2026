# intake-api — submission control plane

The platform's front door. A small Go service (HTTP + a Redis-backed work queue)
that accepts contestant engine submissions and drives them through the sandbox
pipeline (`sandbox/pipeline.sh`: intake → build → attest), then hands attested
submissions off to the microVM/scoring stage.

## API

| Method & path | Purpose |
|---|---|
| `POST /api/v1/submissions` | Multipart upload, field `engine`. Size-checked, sha256'd into an immutable `submission_id`, queued. Returns `202 {submission_id, state:"RECEIVED"}`. |
| `GET  /api/v1/submissions/:id` | Current state-machine snapshot. |
| `GET  /api/v1/submissions` | Newest-first list (id + state). |
| `POST /api/v1/submissions/:id/state` | Orchestrator advance (`RUNNING`, `SCORED`). Guards illegal transitions with `409`. |
| `GET  /health` | `ok` (also pings Redis). |

## State machine

```
RECEIVED → BUILDING → ATTESTED → RUNNING → SCORED
                 ↘ REJECTED  (size cap, banned-syscall scan, or build/attest failure)
```

`RECEIVED → BUILDING → ATTESTED` is driven by the in-process worker (it BRPOPs
`submissions:pending` and runs the pipeline). `RUNNING`/`SCORED` are posted by the
microVM orchestrator that consumes `submissions:scoring`.

## Redis keys

| Key | Type | Role |
|---|---|---|
| `submission:<id>` | hash | per-submission state + metadata |
| `submissions:pending` | list | work queue (LPUSH on receive, BRPOP by worker) |
| `submissions:scoring` | list | handoff to the run/score stage after ATTESTED |
| `submissions:index` | zset | newest-first index for the list endpoint |

## Config (env)

| Var | Default | Notes |
|---|---|---|
| `PORT` | `9090` | |
| `REDIS_ADDR` | `localhost:6379` | |
| `SANDBOX_DIR` | `/app/sandbox` | location of `pipeline.sh` |
| `WORK_DIR` | `/data` | where tarballs + build output live |
| `SIZE_CAP_MB` | `50` | upload cap (matches `intake.sh`) |
| `PIPELINE_SKIP_DOCKER` | `true` | hermetic Docker build needs privileges the API container lacks; the real hermetic build + microVM run happen on the orchestrator |
| `PIPELINE_SKIP_PACK` | `true` | ext4 pack needs root; done on the orchestrator |

## Run locally

```bash
docker run -d --name redis -p 6379:6379 redis:7-alpine
cd platform/intake-api
SANDBOX_DIR=../../sandbox WORK_DIR=/tmp/intake-work go run .

# acceptance check
curl -F engine=@engine.tar.gz localhost:9090/api/v1/submissions
curl localhost:9090/api/v1/submissions/<id>
```

Or via the full stack: `docker compose -f infra/docker-compose.yml up --build`
(the `intake-api` and `redis` services are wired in).
