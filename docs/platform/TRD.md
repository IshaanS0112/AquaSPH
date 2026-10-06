# AquaSPH Platform — Technical Design

Companion to [PRD.md](PRD.md). Decisions with real alternatives are recorded as
ADRs in [adr/](adr/). The HTTP contract is [../../backend/api/openapi.yaml](../../backend/api/openapi.yaml);
a test fails if a registered route is missing from it.

## 1. Topology

```
             ┌──────────────┐  HTTPS/JSON, SSE
  clients ──►│ aquasph-api  │──────────────────┐
  aquactl    │  (N replicas)│                  │ rate limit (GCRA), progress pub/sub
             └──────┬───────┘                  ▼
                    │ SQL                ┌───────────┐
                    ▼                    │  Redis 7  │  optional: degrade, don't fail
             ┌──────────────┐  LISTEN/   └─────▲─────┘
             │ Postgres 16  │  NOTIFY          │ publish progress
             │ jobs = queue │◄──────────┐      │
             └──────────────┘  claim /  │ ┌────┴───────────┐  fork/exec   ┌──────────┐
                               lease /  └─│ aquasph-worker │─────────────►│ aquasph  │
                               result     │  (M replicas)  │◄─JSON lines──│ (C++)    │
                                          └────────┬───────┘   SIGTERM    └──────────┘
                                                   ▼
                                          artifact store (shared volume)
```

- **Postgres is the source of truth and the queue.** A job row *is* the
  queue entry, so enqueue commits in the same transaction as idempotency and
  quota checks ([ADR-0001](adr/0001-postgres-as-queue.md)).
- **The solver runs as a child process, not a linked library**
  ([ADR-0003](adr/0003-solver-process-isolation.md)). A segfault or runaway
  allocation kills one job, not the worker.
- **Redis is an accelerator, not a dependency.** It carries rate-limit state
  and sub-second progress fan-out. Without it the API still serves; SSE polls
  Postgres instead.

## 2. Job state machine

```
            submit                claim (lease)
  (new) ──────────► queued ─────────────────────► running
                     │  ▲                           │ │ │
       cancel        │  │ lease expired,            │ │ └─ exit 0/1 ──► completed (outcome stable|unstable)
       (queued)      │  │ attempts left (reaper)    │ └─── cancel ack ─► cancelled (partial metrics kept)
                     ▼  │ or worker drain           └───── error ──────► failed (error.code)
                 cancelled
```

| Solver exit | Meaning | Job result | Retried? |
|---|---|---|---|
| 0 | STABLE | `completed`, outcome `stable` | — |
| 1 | UNSTABLE | `completed`, outcome `unstable` | No. It is a deterministic physics result. |
| 2 | usage/load error | `failed`, `invalid_scenario` | No. The same input fails again. |
| 3 after user cancel | CANCELLED | `cancelled`, partial metrics | — |
| 3 after timeout | CANCELLED | `failed`, `timeout`, partial metrics | No. It would time out again. |
| 3 after worker drain | CANCELLED | back to `queued`, attempt not charged | Yes |
| killed by a signal we did not send | crash / OOM | `failed`, `solver_crashed` | Yes, up to `max_attempts` |
| lease expired (worker died) | unknown | reaper: `queued` with backoff, or `failed` `lease_expired` | Yes, up to `max_attempts` |

State transitions are written to `job_events` by a **database trigger**, not
by application code, so no code path can forget to audit one.

## 3. Queue semantics

**Claim** (one transaction, `READ COMMITTED`):

1. `SELECT … FROM jobs WHERE state='queued' AND run_after<=now() AND tenant_id <> ALL(saturated)
   ORDER BY priority DESC, id LIMIT 1 FOR UPDATE SKIP LOCKED`
2. `SELECT max_concurrent_jobs FROM tenants WHERE id=$t FOR UPDATE`. This
   serialises claims *per tenant* only.
3. Count the tenant's running jobs. At the limit → roll back, mark the tenant
   saturated, and retry (bounded).
4. Otherwise set `state='running'`, a fresh `lease_token`, `lease_expires_at`,
   `attempt+1`.

Step 2 is what makes the concurrency limit hold under races. Without it, two
workers can each count 1 running job against a limit of 2 and both claim.
Lock order is always job then tenant, and the job lock never waits
(`SKIP LOCKED`), so claims cannot deadlock.

**Fencing.** Every write a worker makes after claiming — heartbeat, progress,
completion — carries `WHERE id=$1 AND lease_token=$2 AND state='running'`. A
worker that stalled past its lease (GC pause, network partition) and wakes up
gets zero rows back. It must kill its solver and discard its result: the job
already belongs to someone else.

**Heartbeat** every `lease/6` renews the lease, stores progress, and returns
`cancel_requested`. One round trip carries three signals, and cancel latency
is bounded by the heartbeat interval.

**Wake-up.** Enqueue runs `pg_notify('aquasph_jobs', …)`, delivered on commit.
Workers `LISTEN` and also poll every `AQUASPH_POLL_INTERVAL` as a safety net,
because NOTIFY is not durable.

**Reaper.** Runs in every worker. It is safe to run concurrently because it
also uses `SKIP LOCKED`. It requeues expired leases with jittered
exponential backoff, fails them after `max_attempts`, and purges idempotency
keys older than 24 h.

## 4. Result cache ([ADR-0002](adr/0002-content-addressed-cache.md))

```
spec_hash = sha256(canonical_json({spec, quality, sim_time, max_steps, max_particles}))
cache_key = sha256(spec_hash + ":" + solver_id)       solver_id = sha256(solver binary)
```

- Thread count is **deliberately excluded** from the key. The solver is
  bit-identical across thread counts (`tests/test_determinism.cpp`), and that
  guarantee is the only reason caching is sound.
- `max_particles` is **included** because the particle ceiling truncates the
  fill and emission, which changes results.
- Lookups happen at submit time (the API uses the `solver_id` of live workers)
  and again at claim time (the worker uses its own), so a duplicate that
  queued behind its twin still hits.
- Scope is set by `AQUASPH_CACHE_SCOPE`: `tenant` (the default), `global`, or
  `off`. `global` shares compute across tenants, at the cost of a timing side
  channel: a fast response reveals that someone ran the same spec.

## 5. Data model

See `backend/internal/db/migrations/`. Key tables:

| Table | Purpose | Notable constraints / indexes |
|---|---|---|
| `tenants` | quotas | `CHECK` on every limit |
| `api_keys` | `prefix` (lookup) + `secret_hash` (SHA-256) | unique prefix, `revoked_at` |
| `jobs` | queue entry + result | partial index `(priority DESC, id) WHERE state='queued'`; `(tenant_id) WHERE state='running'`; `cache_key WHERE state='completed'`; GIN on `labels` |
| `job_events` | audit trail | written by trigger |
| `sweeps` | grid definition | children reference with `ON DELETE CASCADE` |
| `idempotency_keys` | `(tenant_id, key)` PK, request hash | purged after 24 h |
| `workers` | registry: host, solver_id, heartbeat | used by the cache fast path and ops |

IDs are UUIDv7, so they sort by time. Keyset pagination on `id` is therefore
creation-ordered and stable under concurrent inserts, and the cursor is just
the last ID.

## 6. Solver protocol

The worker runs:

```
aquasph --scenario <workdir>/scenario.json --quality Q [--time T] [--steps N]
        --max-particles P --threads K --metrics <workdir>/metrics.json --progress-json
```

With `--progress-json`, stdout carries only JSON lines:

```json
{"event":"start","scenario":"dam_break","tier":1,"quality":"low","fluid":3864,"boundary":13176,"t_end":0.5,"threads":4}
{"event":"progress","t":0.101,"t_end":0.5,"step":430,"dt":2.3e-4,"fluid":3864,"max_speed":1.84,"wall_s":2.0}
{"event":"done","status":"STABLE","t":0.5,"steps":2126,"wall_s":10.5}
```

Progress lines are throttled to one every 0.5 s of wall time. Human-readable
output is suppressed. `SIGTERM` and `SIGINT` set a flag checked at each step
boundary: the solver writes partial metrics with `"status": "CANCELLED"` and
exits 3.

## 7. API surface (summary)

| Method | Path | Notes |
|---|---|---|
| `POST` | `/v1/jobs` | `Idempotency-Key` supported; 201, or a replay of the original response |
| `GET` | `/v1/jobs` | `state`, `scenario`, `sweep_id`, `label=k:v`; cursor pagination |
| `GET` | `/v1/jobs/{id}` | |
| `POST` | `/v1/jobs/{id}/cancel` | 202; queued → cancelled at once; running → cancel requested |
| `GET` | `/v1/jobs/{id}/events` | SSE: `snapshot`, `progress`, `state`, `done` |
| `GET` | `/v1/jobs/{id}/history` | state transitions from `job_events` |
| `GET` | `/v1/jobs/{id}/result` | metrics JSON plus a `partial` flag |
| `GET` | `/v1/jobs/{id}/artifacts[/{name}]` | list; download with Range support |
| `GET` | `/v1/scenarios[/{name}]` | catalogue with tiers |
| `POST` | `/v1/sweeps` | grid of JSON Pointer → values; atomic fan-out |
| `GET` | `/v1/sweeps[/{id}]` | aggregate counts |
| `GET` | `/v1/sweeps/{id}/results` | `format=json\|csv`, `metrics=/ptr,/ptr` |
| `POST` | `/v1/sweeps/{id}/cancel` | |
| `GET` | `/healthz`, `/readyz` | readiness = Postgres reachable |
| `GET` | `/metrics` | internal port only |

Errors are RFC 9457 `application/problem+json`.

**Override rules.** Every override pointer must resolve to a value that
already exists in the base scenario, and the new value must have the same
JSON type. This catches `/numerics/xsph` (a typo the solver would silently
ignore) and `"0.5"` (a string where the solver expects a number) at submit
time, before any compute is spent. Adding an optional field the base omits
requires an inline `spec`.

## 8. Failure modes

| Failure | Behaviour | Where tested |
|---|---|---|
| Worker `SIGKILL`ed mid-job | Lease expires → reaper requeues → another worker completes it | `e2e` failure-injection |
| Worker stalls past lease, then resumes | Fenced writes return 0 rows → solver killed, result discarded | `queue` tests |
| Two workers claim simultaneously | `SKIP LOCKED` → distinct jobs | `queue` concurrency test |
| Tenant at concurrency limit | Its jobs are skipped; other tenants proceed | `queue` fairness test |
| Solver segfault | `failed/solver_crashed`, retried | `solver` tests (fake solver) |
| Solver hangs | Timeout → SIGTERM → grace → SIGKILL | `solver` tests |
| Redis down | Rate limit fails open (`aquasph_ratelimit_errors_total`); SSE polls DB | `ratelimit`, `api` tests |
| Postgres down | `/readyz` 503; workers back off and retry | runbook |
| Duplicate POST (client retry) | Same `Idempotency-Key` → original response replayed | `api` tests |
| Same key, different body | 422 | `api` tests |
| Worker SIGTERM (deploy) | Stop claiming; drain; if the drain times out, cancel the solver and requeue without charging an attempt | `worker` tests |

## 9. Configuration

Everything is an environment variable with a validated default. See
`backend/internal/config/config.go`, which is the single source of truth, and
[runbook.md](runbook.md).

## 10. Extension points (documented, not built)

- **Object storage.** Implement `artifacts.Store` for S3/GCS. The API streams
  artifacts through `Open`, so a signed-URL redirect would be a handler change
  on top.
- **Render jobs.** A second worker class with `kind='render'` claimed through
  the same queue. It needs `aquasph_view --record-headless` plus a GL context
  (a GPU node or Xvfb/llvmpipe).
- **Priority classes per tenant.** Today priority orders globally, bounded by
  per-tenant concurrency. Weighted fair queuing would replace step 1 of the
  claim.
