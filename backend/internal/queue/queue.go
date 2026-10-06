// Package queue implements the durable job queue on top of the jobs table (ADR-0001).
package queue

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/db"
	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/IshaanS0112/AquaSPH/backend/internal/ids"
	"github.com/IshaanS0112/AquaSPH/backend/internal/store"
	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

// NotifyChannel is the LISTEN/NOTIFY channel that wakes idle workers.
const NotifyChannel = store.NotifyChannel

// maxClaimScans bounds how many saturated tenants one Claim call will
// skip past before giving up until the next poll.
const maxClaimScans = 16

type Config struct {
	Lease     time.Duration
	RetryBase time.Duration
	RetryMax  time.Duration
}

type Queue struct {
	pool *pgxpool.Pool
	cfg  Config
}

func New(pool *pgxpool.Pool, cfg Config) *Queue {
	return &Queue{pool: pool, cfg: cfg}
}

func secs(d time.Duration) float64 { return d.Seconds() }

// backoffSQL computes the delay before a retried job becomes claimable: exponential in the
// attempt number, capped, with +/-25% jitter so jobs that failed together (a worker crash takes
// several at once) do not retry together. $base and $max are seconds.
const backoffSQL = `make_interval(secs => LEAST(%[2]s, %[1]s * power(2, GREATEST(attempt - 1, 0)))
	* (0.75 + random() * 0.5))`

func backoff(baseParam, maxParam string) string {
	return fmt.Sprintf(backoffSQL, baseParam, maxParam)
}

// Claim hands the highest-priority claimable job to workerID, or returns (nil, nil) when there
// is none.
func (q *Queue) Claim(ctx context.Context, workerID uuid.UUID) (*domain.Job, error) {
	// Must be non-nil: pgx encodes a nil slice as NULL, and
	// "tenant_id <> ALL(NULL)" is NULL, which would match nothing.
	saturated := []uuid.UUID{}
	for i := 0; i < maxClaimScans; i++ {
		job, full, err := q.claimOnce(ctx, workerID, saturated)
		if err != nil || job != nil {
			return job, err
		}
		if full == nil {
			return nil, nil
		}
		saturated = append(saturated, *full)
	}
	return nil, nil
}

// claimOnce returns (job, nil, nil) on success, (nil, &tenant, nil) when the best candidate's
// tenant is at its limit, and (nil, nil, nil) when nothing is claimable.
func (q *Queue) claimOnce(ctx context.Context, workerID uuid.UUID, saturated []uuid.UUID) (*domain.Job, *uuid.UUID, error) {
	var claimed *domain.Job
	var fullTenant *uuid.UUID
	err := db.InTx(ctx, q.pool, func(tx pgx.Tx) error {
		var id, tenantID uuid.UUID
		// SKIP LOCKED: a row another worker is mid-claim on is invisible here instead of blocking us,
		// so N workers claim N distinct jobs in parallel.
		err := tx.QueryRow(ctx, `
			SELECT id, tenant_id FROM jobs
			WHERE state = 'queued' AND run_after <= now() AND tenant_id <> ALL($1)
			ORDER BY priority DESC, id
			LIMIT 1
			FOR UPDATE SKIP LOCKED`, saturated).Scan(&id, &tenantID)
		if errors.Is(err, pgx.ErrNoRows) {
			return nil
		}
		if err != nil {
			return fmt.Errorf("select candidate: %w", err)
		}

		// Lock the tenant row before counting, or two concurrent claims could both see room
		// under the limit.
		var limit int
		if err := tx.QueryRow(ctx, `SELECT max_concurrent_jobs FROM tenants WHERE id = $1 FOR UPDATE`,
			tenantID).Scan(&limit); err != nil {
			return fmt.Errorf("lock tenant: %w", err)
		}
		// Under READ COMMITTED this statement starts after the lock is
		// held, so it sees every claim committed before ours.
		var running int
		if err := tx.QueryRow(ctx, `SELECT count(*) FROM jobs WHERE tenant_id = $1 AND state = 'running'`,
			tenantID).Scan(&running); err != nil {
			return fmt.Errorf("count running: %w", err)
		}
		if running >= limit {
			fullTenant = &tenantID
			return nil
		}

		claimed, err = store.ScanJob(tx.QueryRow(ctx, `
			UPDATE jobs SET
				state = 'running',
				attempt = attempt + 1,
				lease_token = $2,
				lease_expires_at = now() + make_interval(secs => $3),
				worker_id = $4,
				started_at = COALESCE(started_at, now()),
				progress = NULL
			WHERE id = $1
			RETURNING `+store.JobColumns, id, ids.New(), secs(q.cfg.Lease), workerID))
		if err != nil {
			return fmt.Errorf("claim: %w", err)
		}
		return nil
	})
	if err != nil {
		return nil, nil, err
	}
	return claimed, fullTenant, nil
}

// HeartbeatResult is what one heartbeat round trip tells the worker.
type HeartbeatResult struct {
	CancelRequested bool
}

// Heartbeat renews the lease, stores the latest progress (nil keeps the previous value), and
// reports whether cancellation was requested.
func (q *Queue) Heartbeat(ctx context.Context, jobID, token uuid.UUID, progress json.RawMessage) (HeartbeatResult, error) {
	var res HeartbeatResult
	err := q.pool.QueryRow(ctx, `
		UPDATE jobs SET
			lease_expires_at = now() + make_interval(secs => $3),
			progress = COALESCE($4, progress)
		WHERE id = $1 AND lease_token = $2 AND state = 'running'
		RETURNING cancel_requested_at IS NOT NULL`,
		jobID, token, secs(q.cfg.Lease), nullJSON(progress)).Scan(&res.CancelRequested)
	if errors.Is(err, pgx.ErrNoRows) {
		return res, domain.ErrLeaseLost
	}
	return res, err
}

// Completion is what a successful solver run produces.
type Completion struct {
	Outcome   string
	Result    json.RawMessage
	Artifacts json.RawMessage
	SolverID  string
	CacheKey  string
	Progress  json.RawMessage
}

// Complete records a finished run. Like every write after Claim, it only matches while this
// worker still holds the lease (fencing), so a stale worker cannot overwrite a retry.
func (q *Queue) Complete(ctx context.Context, jobID, token uuid.UUID, c Completion) error {
	return fenced(q.pool.Exec(ctx, `
		UPDATE jobs SET
			state = 'completed', outcome = $3, result = $4, artifacts = $5,
			artifact_job_id = id, solver_id = $6, cache_key = $7,
			progress = COALESCE($8, progress),
			lease_token = NULL, lease_expires_at = NULL, finished_at = now(),
			error_code = NULL, error_message = NULL
		WHERE id = $1 AND lease_token = $2 AND state = 'running'`,
		jobID, token, c.Outcome, nullJSON(c.Result), jsonOr(c.Artifacts, "[]"), c.SolverID, c.CacheKey,
		nullJSON(c.Progress)))
}

// CompleteFromCache completes a claimed job with a previous job's result.
func (q *Queue) CompleteFromCache(ctx context.Context, jobID, token uuid.UUID, source *domain.Job, solverID, cacheKey string) error {
	artifactJob := source.ID
	if source.ArtifactJobID != nil {
		artifactJob = *source.ArtifactJobID
	}
	return fenced(q.pool.Exec(ctx, `
		UPDATE jobs SET
			state = 'completed', outcome = $3, result = $4, artifacts = $5,
			artifact_job_id = $6, cache_hit = true, source_job_id = $7,
			solver_id = $8, cache_key = $9,
			lease_token = NULL, lease_expires_at = NULL, finished_at = now(),
			error_code = NULL, error_message = NULL
		WHERE id = $1 AND lease_token = $2 AND state = 'running'`,
		jobID, token, source.Outcome, nullJSON(source.Result), jsonOr(source.Artifacts, "[]"),
		artifactJob, source.ID, solverID, cacheKey))
}

// Partial is the output of a run that stopped early (timeout, cancel).
type Partial struct {
	Result    json.RawMessage
	Artifacts json.RawMessage
}

// Fail records a failed attempt.
func (q *Queue) Fail(ctx context.Context, jobID, token uuid.UUID, code, message string, retryable bool, p *Partial) (domain.JobState, error) {
	if p == nil {
		p = &Partial{}
	}
	var state string
	err := q.pool.QueryRow(ctx, `
		UPDATE jobs SET
			state = CASE WHEN $5 AND attempt < max_attempts THEN 'queued'::job_state ELSE 'failed'::job_state END,
			run_after = CASE WHEN $5 AND attempt < max_attempts
			                 THEN now() + `+backoff("$8", "$9")+` ELSE run_after END,
			finished_at = CASE WHEN $5 AND attempt < max_attempts THEN NULL ELSE now() END,
			result = CASE WHEN $5 AND attempt < max_attempts THEN result ELSE COALESCE($6, result) END,
			artifacts = CASE WHEN ($5 AND attempt < max_attempts) OR $7::jsonb IS NULL THEN artifacts ELSE $7 END,
			artifact_job_id = CASE WHEN ($5 AND attempt < max_attempts) OR $7::jsonb IS NULL
			                       THEN artifact_job_id ELSE id END,
			error_code = $3, error_message = $4,
			lease_token = NULL, lease_expires_at = NULL
		WHERE id = $1 AND lease_token = $2 AND state = 'running'
		RETURNING state`,
		jobID, token, code, message, retryable, nullJSON(p.Result), nullJSON(p.Artifacts),
		secs(q.cfg.RetryBase), secs(q.cfg.RetryMax)).Scan(&state)
	if errors.Is(err, pgx.ErrNoRows) {
		return "", domain.ErrLeaseLost
	}
	return domain.JobState(state), err
}

// Cancelled records a run stopped at the user's request, keeping whatever
// partial result the solver wrote.
func (q *Queue) Cancelled(ctx context.Context, jobID, token uuid.UUID, p *Partial) error {
	if p == nil {
		p = &Partial{}
	}
	return fenced(q.pool.Exec(ctx, `
		UPDATE jobs SET
			state = 'cancelled', finished_at = now(),
			result = COALESCE($3, result),
			artifacts = COALESCE($4, artifacts),
			artifact_job_id = CASE WHEN $4::jsonb IS NULL THEN artifact_job_id ELSE id END,
			lease_token = NULL, lease_expires_at = NULL
		WHERE id = $1 AND lease_token = $2 AND state = 'running'`,
		jobID, token, nullJSON(p.Result), nullJSON(p.Artifacts)))
}

// Release hands a job back without charging an attempt: the worker is shutting down, which is
// not the job's fault.
func (q *Queue) Release(ctx context.Context, jobID, token uuid.UUID) error {
	return fenced(q.pool.Exec(ctx, `
		UPDATE jobs SET
			state = CASE WHEN cancel_requested_at IS NULL THEN 'queued'::job_state ELSE 'cancelled'::job_state END,
			finished_at = CASE WHEN cancel_requested_at IS NULL THEN NULL ELSE now() END,
			attempt = CASE WHEN cancel_requested_at IS NULL THEN attempt - 1 ELSE attempt END,
			run_after = now(), progress = NULL,
			lease_token = NULL, lease_expires_at = NULL
		WHERE id = $1 AND lease_token = $2 AND state = 'running'`, jobID, token))
}

// ReapResult counts what one Reap pass did.
type ReapResult struct {
	Requeued  int
	Failed    int
	Cancelled int
}

// Reap recovers jobs whose worker stopped heartbeating. SKIP LOCKED makes it safe to run in
// every worker at once.
func (q *Queue) Reap(ctx context.Context, limit int) (ReapResult, error) {
	var res ReapResult
	rows, err := q.pool.Query(ctx, `
		WITH expired AS (
			SELECT id FROM jobs
			WHERE state = 'running' AND lease_expires_at < now()
			ORDER BY lease_expires_at
			LIMIT $1
			FOR UPDATE SKIP LOCKED
		)
		UPDATE jobs j SET
			state = CASE
				WHEN j.cancel_requested_at IS NOT NULL THEN 'cancelled'::job_state
				WHEN j.attempt < j.max_attempts THEN 'queued'::job_state
				ELSE 'failed'::job_state END,
			finished_at = CASE
				WHEN j.cancel_requested_at IS NULL AND j.attempt < j.max_attempts THEN NULL
				ELSE now() END,
			run_after = CASE
				WHEN j.cancel_requested_at IS NULL AND j.attempt < j.max_attempts
				THEN now() + `+backoff("$2", "$3")+`
				ELSE j.run_after END,
			error_code = CASE WHEN j.cancel_requested_at IS NOT NULL THEN j.error_code ELSE 'lease_expired' END,
			error_message = CASE WHEN j.cancel_requested_at IS NOT NULL THEN j.error_message
				ELSE 'worker stopped heartbeating; lease expired' END,
			lease_token = NULL, lease_expires_at = NULL, progress = NULL
		FROM expired
		WHERE j.id = expired.id
		RETURNING j.state`, limit, secs(q.cfg.RetryBase), secs(q.cfg.RetryMax))
	if err != nil {
		return res, err
	}
	defer rows.Close()
	for rows.Next() {
		var s string
		if err := rows.Scan(&s); err != nil {
			return res, err
		}
		switch domain.JobState(s) {
		case domain.StateQueued:
			res.Requeued++
		case domain.StateFailed:
			res.Failed++
		case domain.StateCancelled:
			res.Cancelled++
		}
	}
	return res, rows.Err()
}

// PurgeIdempotencyKeys deletes keys older than maxAge.
func (q *Queue) PurgeIdempotencyKeys(ctx context.Context, maxAge time.Duration) (int64, error) {
	tag, err := q.pool.Exec(ctx, `DELETE FROM idempotency_keys WHERE created_at < now() - make_interval(secs => $1)`,
		secs(maxAge))
	return tag.RowsAffected(), err
}

// Depth returns the number of jobs in each state.
func (q *Queue) Depth(ctx context.Context) (map[domain.JobState]int64, error) {
	out := map[domain.JobState]int64{}
	for _, s := range domain.AllStates {
		out[s] = 0
	}
	rows, err := q.pool.Query(ctx, `SELECT state, count(*) FROM jobs GROUP BY state`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	for rows.Next() {
		var s string
		var n int64
		if err := rows.Scan(&s, &n); err != nil {
			return nil, err
		}
		out[domain.JobState(s)] = n
	}
	return out, rows.Err()
}

// OldestQueuedAge is the wait time of the longest-waiting claimable job,
// the single best alert signal for "workers are not keeping up".
func (q *Queue) OldestQueuedAge(ctx context.Context) (time.Duration, error) {
	var seconds *float64
	err := q.pool.QueryRow(ctx, `
		SELECT EXTRACT(EPOCH FROM now() - min(created_at))::float8
		FROM jobs WHERE state = 'queued' AND run_after <= now()`).Scan(&seconds)
	if err != nil || seconds == nil {
		return 0, err
	}
	return time.Duration(*seconds * float64(time.Second)), nil
}

func fenced(tag interface{ RowsAffected() int64 }, err error) error {
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return domain.ErrLeaseLost
	}
	return nil
}

// nullJSON maps an empty RawMessage to SQL NULL so COALESCE keeps the
// existing value instead of storing invalid JSON.
func nullJSON(b json.RawMessage) any {
	if len(b) == 0 {
		return nil
	}
	return string(b)
}

func jsonOr(b json.RawMessage, def string) string {
	if len(b) == 0 {
		return def
	}
	return string(b)
}
