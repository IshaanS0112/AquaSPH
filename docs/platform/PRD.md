# AquaSPH Platform — Product Requirements

**Status:** implemented. [TRD.md](TRD.md) covers how,
[verification.md](verification.md) covers what was exercised on the running
system, and [benchmarks.md](benchmarks.md) has the measurements.

## 1. The problem

AquaSPH v2 is a deterministic 3D SPH solver with a scenario library. As a
command-line tool it has three limits that matter the moment more than one
person, or more than one experiment, is involved:

1. **It runs where you type.** A 60-second medium-quality run blocks a
   terminal. A 40-point parameter study is a shell loop that dies with the
   laptop lid.
2. **Results are files on one disk.** Nothing records who ran what, with which
   overrides, against which solver build, or whether an identical run already
   exists.
3. **No isolation or limits.** One user can start a run that takes every core
   for an hour. There is no quota, no fairness, no cancellation other than
   `kill`, and `kill` discards everything computed so far.

## 2. Who it is for

| User | What they need |
|---|---|
| **Experimenter** (student, researcher) | Submit a scenario with overrides, watch progress live, get metrics back, run parameter sweeps without scripting. |
| **Integrator** (a notebook, a CI job, another service) | A stable HTTP API with idempotent submission, machine-readable results, and pagination that does not skip or repeat rows. |
| **Operator** (whoever runs the deployment) | Quotas per tenant, horizontal worker scaling, crash recovery without manual intervention, metrics to alert on, a runbook. |

## 3. Scope

### In scope (v1 of the platform)

| # | Requirement | Verified by |
|---|---|---|
| R1 | Submit a job: named scenario + JSON-Pointer overrides, or a full inline spec. | API + E2E tests |
| R2 | Jobs run asynchronously on a horizontally scalable worker pool. | E2E, multi-worker test |
| R3 | Live progress over Server-Sent Events. | SSE integration test |
| R4 | Cancellation that keeps the partial result (solver stops at a step boundary). | Solver ctest + E2E |
| R5 | Parameter sweeps: a cartesian grid fanned out into child jobs, results as JSON or CSV. | API + E2E tests |
| R6 | Content-addressed result cache — an identical request returns in milliseconds. | Cache tests |
| R7 | Multi-tenancy: API keys, per-tenant quotas (concurrent, queued, particles, wall time). | Queue + API tests |
| R8 | Rate limiting per API key. | Rate-limit tests |
| R9 | Idempotent submission via `Idempotency-Key`. | API tests |
| R10 | Crash recovery: a worker that dies mid-job loses its lease, and the job is retried. | Failure-injection test |
| R11 | Prometheus metrics, structured logs, health/readiness probes. | Handler tests |
| R12 | One-command local deployment (Docker Compose) and CI against real Postgres/Redis. | Compose file, CI job |

### Explicitly out of scope — and why

| Not built | Reason |
|---|---|
| Web UI | This is a backend project. The API, `aquactl`, and SSE are the interfaces. |
| S3/object storage | The `artifacts.Store` interface is the seam. No S3-compatible service exists in the build environment to test against, and an untested S3 backend is worse than none. |
| Kubernetes manifests | Compose proves the topology. Manifests nobody has applied are decoration. |
| GPU rendering jobs | The renderer needs a GL context. A render worker is a documented extension (TRD §10). |
| OAuth / SSO | API keys are the correct primitive for machine clients. User login belongs to a UI that does not exist. |
| Billing | Quotas are enforced. Metering for money is a separate product decision. |

## 4. Success metrics

These are properties the system must demonstrably have, not usage targets:

- **Exactly-once completion under concurrency.** N workers racing on M jobs
  complete each job once. Tested with 8 workers and 200 jobs.
- **No lost jobs.** A worker killed with `SIGKILL` mid-run: its job returns to
  the queue after the lease expires and completes on another worker.
- **Tenant limits hold under races.** A tenant with a concurrency limit of 2 is
  never observed running 3 jobs, even when 8 workers claim at once.
- **Cache correctness.** A cache hit returns metrics byte-identical to a fresh
  run. This rests on the solver's bit-identical-across-thread-count guarantee,
  and the platform test re-checks it.
- **API overhead is measured, not estimated.** See [benchmarks.md](benchmarks.md).

## 5. Non-functional requirements

- **Security:** API keys stored as SHA-256 hashes, never in plaintext. Tenant
  isolation enforced in every query (tested). Request bodies are
  size-limited. Artifact names are allowlisted, so there is no path traversal.
- **Operability:** 12-factor configuration through environment variables.
  Graceful shutdown on SIGTERM. Every failure mode has a runbook entry.
- **Degradation:** if Redis is down, rate limiting fails open (and says so in
  a metric), and SSE falls back to database polling. Postgres is the only hard
  dependency.
