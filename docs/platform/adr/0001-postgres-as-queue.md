# ADR-0001: Postgres is the job queue

**Status:** accepted

## Context
Jobs need durable storage, a queue that workers claim from, per-tenant
concurrency limits, idempotent submission, and cancellation. Throughput is
bounded by the solver: a job takes seconds to minutes, so the queue sees
single-digit claims per second, not thousands.

## Decision
The `jobs` table is the queue. Workers claim with `SELECT … FOR UPDATE SKIP
LOCKED`, hold a time-bounded lease, and are fenced by a `lease_token`.
`LISTEN/NOTIFY` wakes idle workers, with polling as a fallback.

## Rejected alternatives
| Option | Why not |
|---|---|
| **Redis list / Streams** | Enqueue would be a second write outside the Postgres transaction that creates the job row and records the idempotency key. Either write can fail after the other commits: a phantom job, or a job nobody runs. Fixing that needs an outbox, which reinvents a Postgres queue. |
| **RabbitMQ / Kafka** | A new stateful service for a workload of a few claims per second. It still does not express "at most N running per tenant", which needs a view of running jobs that the broker does not have. |
| **Asynq / River / other queue libraries** | River is Postgres-backed and good. But the claim query, lease, and fence are the interesting parts of this project, so they are written here in ~150 lines of reviewed SQL rather than hidden in a dependency. |

## Consequences
- Enqueue, quota checks, and idempotency are one atomic transaction.
- Queue depth is a SQL query, and the audit trail is a trigger.
- Ceiling: the claim path serialises per tenant and touches a partial index.
  It will not reach tens of thousands of claims per second. It does not need
  to: that is roughly five orders of magnitude above what the solver can
  consume.
