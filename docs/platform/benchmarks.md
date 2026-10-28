# Platform benchmarks

Every number here was measured; nothing is extrapolated. Each figure
lists the machine it came from, because a latency without its hardware
means nothing.

## Setup

| | |
|---|---|
| Machine | One VM: 4 vCPU (Intel Xeon @ 2.10 GHz). It hosted **everything at once**: API, Postgres 16, Redis 7, two idle workers, Docker's userland port proxy, and the load generator itself. |
| Deployment | `docker compose up` from this repository, with Docker's `vfs` storage driver. Postgres at stock settings (fsync on). |
| Tool | `aquasph-loadgen` (`backend/cmd/aquasph-loadgen`), 32 concurrent keep-alive clients, 3 s warm-up discarded, 20 s measured. |
| Solver | Not running during the measurement. Checked beforehand: `pgrep aquasph` was empty, so no CPU contention from simulations. |

## API overhead

| Path | What it exercises | Throughput | p50 | p90 | p99 | max | Errors |
|---|---|---|---|---|---|---|---|
| `GET /v1/jobs/{id}` | auth (key lookup + constant-time hash compare), GCRA rate limit in Redis, one indexed read | **2,852 req/s** | 10.8 ms | 14.6 ms | 20.1 ms | 54 ms | 0 / 57,038 |
| `POST /v1/jobs` (cache hit) | the full submit path: auth, rate limit, strict decode, JSON Pointer resolution, canonical hashing, live-solver lookup, cache lookup, transactional insert with the audit trigger, commit | **477 req/s** | 67.0 ms | 85.3 ms | 114.3 ms | 161 ms | 0 / 9,545 |
| `GET /v1/jobs?limit=20` | keyset pagination, 21 rows | **756 req/s** | 40.0 ms | 55.0 ms | 73.7 ms | 110 ms | 0 / 15,122 |

The submit benchmark uses a request that is already cached, so it
measures the platform with zero solver time. `aquasph-loadgen` refuses
to run in submit mode unless it has confirmed that first, so a benchmark
can never flood the queue with real simulations.

### How to read these

- **The solver dominates.** A low-quality 0.3 s dam break takes ~11 s on
  two threads, and a medium-quality 2 s run took ~14 minutes. A 67 ms
  submit is 0.6% of the former and ~0.008% of the latter. Only for
  deliberately tiny jobs does the overhead show: a 0.05 s run takes
  ~1.8 s, of which a submit is ~4%. The API is not the bottleneck and
  there is no reason to optimise it yet.
- **Everything shared four cores.** The load generator's 32 clients
  competed with the API and Postgres for the same CPUs, so these are
  lower bounds for a deployment where they run on separate machines.
- **Submit is write-bound.** Each submission is one transaction with an
  insert, an audit-trigger insert and a commit (fsync). Little's law
  checks out: 32 clients / 477 req/s ≈ 67 ms, the measured p50.
- **The first optimisation target, if one is ever needed:** the list
  query selects the full `spec` column (~1.4 KB per job) and then
  discards it. It has not been changed, because the numbers do not
  justify it.

## Cache and cancellation (latencies of single operations)

| Operation | Measured | Where |
|---|---|---|
| Fresh `dam_break`, low quality, 0.3 s simulated, 2 threads | 11.05 s | live run, `aquactl submit --watch` |
| The same request again (cache hit), client round trip | **15.3 ms** | live run |
| Cache hit inside the E2E suite | 6.5 ms | `backend/e2e`, logged by the test |
| Cancel request → solver stopped → job `cancelled` | **0.81 s** | live run, 1 s heartbeat |
| `docker compose stop worker` mid-job (drain lets it finish) | 4 s | Compose stack |

The cached result's metrics were checked to be byte-identical to the
original's. That is a property of the solver: its output is
bit-identical across thread counts (`tests/test_determinism.cpp`).

## Recovery (failure injection, live system)

A medium-quality job (21,525 fluid particles, 2 s simulated) had its
worker `SIGKILL`ed twice. The audit trail, written by the database
trigger:

```
  +  0.00s  None      -> queued    attempt 0
  +  0.01s  queued    -> running   attempt 1
  +  8.48s  running   -> queued    attempt 1  lease_expired     <- worker 1 SIGKILLed
  + 10.48s  queued    -> running   attempt 2
  + 46.86s  running   -> queued    attempt 2  lease_expired     <- worker 2 SIGKILLed
  + 50.87s  queued    -> running   attempt 3
  +894.05s  running   -> completed attempt 3                    STABLE, 14,989 steps
```

Recovery time is the lease (6 s in that run) plus the reap interval
(2 s) plus backoff. With the defaults (30 s lease, 10 s reap interval)
expect ~40 s. The killed worker's solver was sampled at 20 ms intervals
from the instant of the kill: it was already a zombie (dead, 0% CPU) at
t+0 ms, killed by `Pdeathsig`.

## Reproduce

```bash
make up && make demo-key                      # prints a key
docker compose exec -T api aquasph-admin tenant update demo \
    --rate-per-minute 100000000 --rate-burst 10000000
export AQUASPH_API_KEY=<key>
make loadtest
```
