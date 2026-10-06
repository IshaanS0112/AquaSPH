# Operations runbook

For whoever runs the platform. Design rationale is in [TRD.md](TRD.md);
this page covers what to watch, what breaks, and what to do about it.

## Services

| Process | Scales | State | Ports |
|---|---|---|---|
| `aquasph-api` | horizontally, stateless | none (Postgres is the source of truth) | 8080 public; 9090 internal (`/metrics`, `/healthz`, `/readyz`) |
| `aquasph-worker` | horizontally | one leased job per slot | 9091 internal (`/metrics`, `/healthz`) |
| Postgres 16 | vertically | jobs (= the queue), tenants, keys, audit trail | 5432 |
| Redis 7 | not needed for correctness | rate-limit counters, progress fan-out | 6379 |

Both binaries migrate the schema on start. That is safe with any number
of replicas: an advisory lock serialises migrators, and a migration file
edited after it was applied stops the process with an error.

## Sizing workers

The solver is OpenMP-parallel, so one job can use every core. On a
machine with **C** cores:

```
AQUASPH_WORKER_CONCURRENCY × AQUASPH_SOLVER_THREADS ≤ C
```

Oversubscribing does not raise throughput; it only makes every job
slower. For throughput on many small jobs (sweeps), use more slots with
fewer threads each. For latency on single large jobs, use one slot with
all cores. Thread count never changes results (the solver is
bit-identical across thread counts), so this is purely a throughput
decision.

## Alerts

| Alert | Expression (PromQL) | Meaning |
|---|---|---|
| Workers not keeping up | `aquasph_queue_oldest_age_seconds > 300` | The oldest claimable job has waited 5 min. Add workers, or check `aquasph_worker_busy_slots`. |
| Jobs dying mid-run | `increase(aquasph_reaper_jobs_total{action="requeued"}[15m]) > 0` | Workers are vanishing without releasing their leases (OOM kills, node loss). Check worker restarts and memory. |
| Retries exhausted | `increase(aquasph_worker_jobs_total{result=~"failed_solver_crashed\|failed_internal"}[1h]) > 0` | A job failed after `max_attempts`. Read its `error.message` and `stderr.log` artifact. |
| Rate limiter degraded | `increase(aquasph_ratelimit_errors_total[5m]) > 0` | Redis is unreachable; requests are being allowed through unthrottled (fail-open). |
| API errors | `sum(rate(aquasph_http_requests_total{code=~"5.."}[5m])) > 0` | Each 5xx is logged with its `request_id`; clients see the same ID. |
| Zombie attempts | `increase(aquasph_worker_jobs_total{result="lease_lost"}[1h]) > 0` | A worker lost its lease while alive: heartbeats are not getting through (DB overload, network partition, a long stall). The fencing token made the stale write harmless, but find the cause. |

## Failure modes

**A worker dies (OOM, node lost, `kill -9`).**
Nothing to do. Its leases expire, the reaper in any surviving worker
requeues the jobs with backoff, and another worker picks them up. The
killed worker's solver dies with it (`Pdeathsig`). Verified live; see
[benchmarks.md](benchmarks.md#recovery-failure-injection-live-system).
In containers, run workers with an init process (Compose sets
`init: true`); otherwise dead solvers linger as zombies, because the
worker is not an init and nothing reaps orphans.

**Deploying workers.**
`SIGTERM` makes a worker stop claiming and gives running jobs
`AQUASPH_DRAIN_TIMEOUT` (default 20 s) to finish. Jobs still running at
the deadline are stopped and released back to the queue *without*
charging an attempt. Set the orchestrator's grace period above the drain
timeout (Compose: `stop_grace_period: 30s`); otherwise the kill arrives
mid-drain and every deploy turns into lease expiries.

**Redis is down.**
The API keeps serving. Rate limiting fails open (visible in
`aquasph_ratelimit_errors_total`), SSE progress falls back to polling
Postgres once a second, and workers keep publishing into the void.
Nothing is lost, because nothing in Redis is the only copy of anything.

**Postgres is down.**
`/readyz` returns 503, so load balancers stop routing to the API.
Workers log claim errors and retry every poll interval. Running solvers
continue, but their heartbeats fail, so if the outage outlasts the lease
their jobs are re-run once the database returns. Re-running wastes
compute; it never produces a wrong result.

**A job is stuck in `running`.**
It cannot be for longer than the lease: either its worker is heartbeating
(it really is running; check `progress`) or the reaper will requeue it.
`aquasph-admin workers` shows who is alive and when each last beat.

**A solver build changes.**
The worker's `solver_id` is the SHA-256 of the solver binary, and it is
part of every cache key. A new build therefore never serves results
computed by an old one. During a rolling deploy both builds are live and
the API may serve a cached result from either; each job records the
`solver_id` that produced it.

## Tenants and keys

```bash
aquasph-admin tenant create acme --max-concurrent 4 --max-particles 500000
aquasph-admin key create acme --name ci           # printed once; only its hash is stored
aquasph-admin key list acme
aquasph-admin key revoke <prefix>                 # takes effect on the next request
aquasph-admin tenant update acme --rate-per-minute 1200
aquasph-admin queue                               # counts by state, oldest wait
```

## Configuration

All environment variables, with validated defaults. A misconfigured
process lists *every* problem and exits, rather than failing on the first.

| Variable | Default | Used by |
|---|---|---|
| `AQUASPH_DATABASE_URL` | (required) | all |
| `AQUASPH_REDIS_URL` | empty: Redis features off | api, worker |
| `AQUASPH_HTTP_ADDR` / `AQUASPH_INTERNAL_ADDR` | `:8080` / `:9090` (api), `:9091` (worker) | api, worker |
| `AQUASPH_ARTIFACT_DIR` | `./data/artifacts` (must be shared by api and workers) | api, worker |
| `AQUASPH_SCENARIO_DIR` | `../configs/scenarios` | api |
| `AQUASPH_CACHE_SCOPE` | `tenant` (`global` shares results across tenants, `off` disables) | api, worker |
| `AQUASPH_MAX_SWEEP_SIZE` | 256 | api |
| `AQUASPH_MAX_BODY_BYTES` | 262144 | api |
| `AQUASPH_SSE_MAX_DURATION` | 30m | api |
| `AQUASPH_SOLVER_PATH` | `../build/aquasph` | worker |
| `AQUASPH_SOLVER_THREADS` | 0 (OpenMP decides) | worker |
| `AQUASPH_WORKER_CONCURRENCY` | 1 | worker |
| `AQUASPH_LEASE_DURATION` | 30s | worker |
| `AQUASPH_HEARTBEAT_INTERVAL` | 5s (at most lease/3, enforced) | worker |
| `AQUASPH_POLL_INTERVAL` | 2s (LISTEN/NOTIFY usually wakes workers sooner) | worker |
| `AQUASPH_REAP_INTERVAL` | 10s | worker |
| `AQUASPH_DRAIN_TIMEOUT` | 20s | worker |
| `AQUASPH_CANCEL_GRACE` | 10s (SIGTERM → SIGKILL) | worker |
| `AQUASPH_RETRY_BASE` / `AQUASPH_RETRY_MAX` | 5s / 5m, with ±25% jitter | worker |
| `AQUASPH_LOG_LEVEL` / `AQUASPH_LOG_FORMAT` | `info` / `json` | all |

## Not built (see TRD §10)

Artifact retention and garbage collection, object storage, and
Kubernetes manifests. Artifacts accumulate on the shared volume until
someone deletes them.
