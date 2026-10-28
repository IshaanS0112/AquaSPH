# ADR-0005: Webhooks through a transactional outbox

**Status:** accepted

## Context
Tenants poll `GET /v1/jobs/{id}` or hold an SSE stream open to learn that a job finished. A
service that submits a sweep of 200 jobs wants to be told instead. Pushing an HTTP request to a
tenant's URL has two hard parts:

- **Consistency with the job.** A job reaches a terminal state on nine code paths: the worker's
  complete, fail and cancel writes, its own cache hit, a release during drain that finds a
  cancel request, the reaper, a cache hit at submit time, and cancelling a queued job or a whole
  sweep. An event sent for a transition that then rolled back is a lie. An event never sent for
  a committed transition is a silent gap.
- **Delivery.** Receivers are down, slow, or return 500. Any number of API replicas may be
  dispatching at once, and a replica can die mid-request.

## Decision
An `AFTER INSERT OR UPDATE OF state` trigger on `jobs` writes an `outbox_events` row and one
`webhook_deliveries` row per subscribed endpoint, in the same transaction as the transition.
This is the pattern already used for the `job_events` audit trail. The event exists exactly
when the transition commits, whichever of the nine paths made it.

A dispatcher in the API process delivers the rows. It reuses the job queue's design
([ADR-0001](0001-postgres-as-queue.md)):

- It claims due rows with `FOR UPDATE SKIP LOCKED`, so replicas never send the same row at the
  same time.
- Each claim takes a lease and increments the row's attempt number. The attempt number is the
  fencing token: a dispatcher that stalls past its lease cannot record over a newer attempt.
- It is woken by `NOTIFY aquasph_webhooks` from the trigger, with polling as a fallback.
- Failures retry with exponential backoff and equal jitter (10 s doubling to a 1 h cap). After
  8 attempts the row is dead-lettered. `POST …/redeliver` sends it again by hand.

Each request is signed `AquaSPH-Signature: t=<unix>,v1=HMAC-SHA256(secret, "<t>.<body>")`.
Signing the timestamp lets a receiver reject replays. The envelope `id` is the event's id, the
same on every retry, so receivers deduplicate on it.

## Delivery guarantee
At least once, never "exactly once". A receiver can answer 200 just as the dispatcher dies,
before the outcome is recorded; the lease then expires and the event is sent again. Exactly
once would need the receiver to take part in a two-phase protocol. Deduplicating on the event
id gives the receiver the same effect for less.

## Security: the dispatcher is an SSRF primitive
The tenant chooses the URL, so the platform makes requests wherever the tenant points it:
cloud metadata at `169.254.169.254`, Postgres on localhost, the internal metrics port. The
defences:

- Registration requires https and refuses hosts that are loopback, private, link-local, CGNAT
  or otherwise reserved IP literals.
- The decisive check runs at **dial time**: a `net.Dialer` `Control` hook rejects the resolved
  address. A name that resolves publicly at registration and privately at delivery (DNS
  rebinding) is caught there.
- Redirects are not followed, so a public URL cannot bounce the request inward.
- Proxy environment variables are ignored. Behind a proxy the dial-time check would see the
  proxy's address, not the target's.

`AQUASPH_WEBHOOKS_ALLOW_INSECURE=true` lifts these limits for local development only.

## Rejected alternatives
| Option | Why not |
|---|---|
| **Send from application code after commit** | Nine call sites to keep in sync, and a crash between commit and send loses the event. This is the dual-write problem the outbox exists to solve. |
| **Logical decoding / Debezium** | Reads the WAL, so it captures every change with no trigger. But it is a new service and a replication slot to operate, for a few events per second. |
| **Redis Streams as the queue** | The same dual-write problem as in ADR-0001: the event must be durable in the transaction that creates it. |
| **Store only a hash of the secret** | That works for API keys (ADR-0004), where the server only verifies. Here the server signs, so it needs the secret itself. The secret is shown once, at creation. |

## Consequences
- Adding an event type means a branch in one trigger function. No handler can forget to emit.
- `outbox_events` doubles as a per-tenant activity log; rows are purged after 30 days.
- The dispatcher costs one connection for `LISTEN` per API replica, plus pooled queries.
