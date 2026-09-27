package store

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"strings"

	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/google/uuid"
)

// NewJob is everything needed to insert a job. A job inserted with
// CacheSource set is created already completed (a cache hit); otherwise it
// is queued.
type NewJob struct {
	ID             uuid.UUID
	TenantID       uuid.UUID
	SweepID        *uuid.UUID
	SweepParams    json.RawMessage
	Priority       int
	Scenario       string
	Quality        string
	SimTime        *float64
	MaxSteps       *int
	MaxParticles   int
	TimeoutSeconds int
	Spec           json.RawMessage
	Overrides      json.RawMessage
	Labels         json.RawMessage
	SpecHash       string
	AllowCache     bool

	CacheSource *domain.Job
}

func rawOr(b json.RawMessage, def string) string {
	if len(b) == 0 {
		return def
	}
	return string(b)
}

func nullRaw(b json.RawMessage) any {
	if len(b) == 0 {
		return nil
	}
	return string(b)
}

// InsertJob inserts a job within the caller's transaction.
func InsertJob(ctx context.Context, q Querier, n *NewJob) (*domain.Job, error) {
	if src := n.CacheSource; src != nil {
		artifactJob := src.ID
		if src.ArtifactJobID != nil {
			artifactJob = *src.ArtifactJobID
		}
		return ScanJob(q.QueryRow(ctx, `
			INSERT INTO jobs (id, tenant_id, sweep_id, sweep_params, priority, scenario, quality, sim_time,
				max_steps, max_particles, timeout_seconds, spec, overrides, labels, spec_hash, allow_cache,
				state, outcome, result, artifacts, artifact_job_id, cache_hit, source_job_id, solver_id,
				cache_key, started_at, finished_at)
			VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16,
				'completed', $17, $18, $19, $20, true, $21, $22, $23, now(), now())
			RETURNING `+JobColumns,
			n.ID, n.TenantID, n.SweepID, nullRaw(n.SweepParams), n.Priority, n.Scenario, n.Quality, n.SimTime,
			n.MaxSteps, n.MaxParticles, n.TimeoutSeconds, string(n.Spec), rawOr(n.Overrides, "{}"),
			rawOr(n.Labels, "{}"), n.SpecHash, n.AllowCache,
			src.Outcome, nullRaw(src.Result), rawOr(src.Artifacts, "[]"), artifactJob, src.ID,
			src.SolverID, src.CacheKey))
	}
	return ScanJob(q.QueryRow(ctx, `
		INSERT INTO jobs (id, tenant_id, sweep_id, sweep_params, priority, scenario, quality, sim_time,
			max_steps, max_particles, timeout_seconds, spec, overrides, labels, spec_hash, allow_cache)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16)
		RETURNING `+JobColumns,
		n.ID, n.TenantID, n.SweepID, nullRaw(n.SweepParams), n.Priority, n.Scenario, n.Quality, n.SimTime,
		n.MaxSteps, n.MaxParticles, n.TimeoutSeconds, string(n.Spec), rawOr(n.Overrides, "{}"),
		rawOr(n.Labels, "{}"), n.SpecHash, n.AllowCache))
}

// LockTenantQuota serialises quota checks for one tenant until the
// transaction ends. It is an advisory lock rather than a lock on the
// tenant row, so submissions never contend with worker claims (which do
// lock the tenant row).
func LockTenantQuota(ctx context.Context, q Querier, tenantID uuid.UUID) error {
	_, err := q.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended('quota:' || $1::text, 0))`, tenantID)
	return err
}

func CountQueued(ctx context.Context, q Querier, tenantID uuid.UUID) (int, error) {
	var n int
	err := q.QueryRow(ctx, `SELECT count(*) FROM jobs WHERE tenant_id = $1 AND state = 'queued'`, tenantID).Scan(&n)
	return n, err
}

// FindCached returns the most recent completed job matching any of the
// cache keys, or nil. tenantID nil searches across tenants (scope=global).
func FindCached(ctx context.Context, q Querier, cacheKeys []string, tenantID *uuid.UUID) (*domain.Job, error) {
	if len(cacheKeys) == 0 {
		return nil, nil
	}
	j, err := ScanJob(q.QueryRow(ctx, `
		SELECT `+JobColumns+` FROM jobs
		WHERE state = 'completed' AND cache_key = ANY($1)
		  AND ($2::uuid IS NULL OR tenant_id = $2)
		ORDER BY finished_at DESC
		LIMIT 1`, cacheKeys, tenantID))
	if err != nil {
		if errors.Is(notFound(err), domain.ErrNotFound) {
			return nil, nil
		}
		return nil, err
	}
	return j, nil
}

// FindCachedMany is FindCached for a batch (sweep submission): it returns
// spec_hash -> source job for every hash with a cached result.
func FindCachedMany(ctx context.Context, q Querier, keyToHash map[string]string, tenantID *uuid.UUID) (map[string]*domain.Job, error) {
	out := map[string]*domain.Job{}
	if len(keyToHash) == 0 {
		return out, nil
	}
	keys := make([]string, 0, len(keyToHash))
	for k := range keyToHash {
		keys = append(keys, k)
	}
	rows, err := q.Query(ctx, `
		SELECT DISTINCT ON (cache_key) `+JobColumns+` FROM jobs
		WHERE state = 'completed' AND cache_key = ANY($1)
		  AND ($2::uuid IS NULL OR tenant_id = $2)
		ORDER BY cache_key, finished_at DESC`, keys, tenantID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	for rows.Next() {
		j, err := ScanJob(rows)
		if err != nil {
			return nil, err
		}
		out[keyToHash[*j.CacheKey]] = j
	}
	return out, rows.Err()
}

func (s *Store) GetJob(ctx context.Context, tenantID, id uuid.UUID) (*domain.Job, error) {
	j, err := ScanJob(s.pool.QueryRow(ctx, `SELECT `+JobColumns+` FROM jobs WHERE id = $1 AND tenant_id = $2`,
		id, tenantID))
	return j, notFound(err)
}

// GetJobUnscoped is for workers and internal paths only; never call it
// with an ID taken from a request.
func (s *Store) GetJobUnscoped(ctx context.Context, id uuid.UUID) (*domain.Job, error) {
	j, err := ScanJob(s.pool.QueryRow(ctx, `SELECT `+JobColumns+` FROM jobs WHERE id = $1`, id))
	return j, notFound(err)
}

type JobFilter struct {
	State    *domain.JobState
	Scenario string
	SweepID  *uuid.UUID
	Labels   map[string]string
}

// Cursor is an opaque keyset-pagination token. It encodes the last ID of
// the previous page; because IDs are UUIDv7, "id < cursor" is "created
// before", and a page boundary cannot skip or repeat rows when new jobs
// are inserted concurrently, which OFFSET pagination would.
type Cursor = string

func EncodeCursor(id uuid.UUID) Cursor { return base64.RawURLEncoding.EncodeToString(id[:]) }

func DecodeCursor(c Cursor) (uuid.UUID, error) {
	b, err := base64.RawURLEncoding.DecodeString(c)
	if err != nil || len(b) != 16 {
		return uuid.Nil, fmt.Errorf("invalid cursor")
	}
	return uuid.UUID(b), nil
}

// ListJobs returns up to limit jobs, newest first, and the cursor for the
// next page ("" when this is the last page).
func (s *Store) ListJobs(ctx context.Context, tenantID uuid.UUID, f JobFilter, after *uuid.UUID, limit int) ([]*domain.Job, Cursor, error) {
	where := []string{"tenant_id = $1"}
	args := []any{tenantID}
	add := func(cond string, v any) {
		args = append(args, v)
		where = append(where, fmt.Sprintf(cond, len(args)))
	}
	if f.State != nil {
		add("state = $%d", string(*f.State))
	}
	if f.Scenario != "" {
		add("scenario = $%d", f.Scenario)
	}
	if f.SweepID != nil {
		add("sweep_id = $%d", *f.SweepID)
	}
	if len(f.Labels) > 0 {
		b, _ := json.Marshal(f.Labels)
		add("labels @> $%d::jsonb", string(b)) // served by the GIN index
	}
	if after != nil {
		add("id < $%d", *after)
	}
	args = append(args, limit+1) // one extra row tells us whether there is a next page
	rows, err := s.pool.Query(ctx, fmt.Sprintf(`SELECT %s FROM jobs WHERE %s ORDER BY id DESC LIMIT $%d`,
		JobColumns, strings.Join(where, " AND "), len(args)), args...)
	if err != nil {
		return nil, "", err
	}
	defer rows.Close()
	var out []*domain.Job
	for rows.Next() {
		j, err := ScanJob(rows)
		if err != nil {
			return nil, "", err
		}
		out = append(out, j)
	}
	if err := rows.Err(); err != nil {
		return nil, "", err
	}
	var next Cursor
	if len(out) > limit {
		out = out[:limit]
		next = EncodeCursor(out[limit-1].ID)
	}
	return out, next, nil
}

// CancelJob cancels a queued job immediately, or flags a running one for
// its worker, in one statement. A worker's claim holds the row lock, so a
// cancel racing a claim waits, then re-evaluates the CASE against the
// committed state: it can never cancel a job "as queued" that a worker
// has just started.
func (s *Store) CancelJob(ctx context.Context, tenantID, id uuid.UUID) (*domain.Job, error) {
	j, err := ScanJob(s.pool.QueryRow(ctx, `
		UPDATE jobs SET
			state = CASE WHEN state = 'queued' THEN 'cancelled'::job_state ELSE state END,
			finished_at = CASE WHEN state = 'queued' THEN now() ELSE finished_at END,
			cancel_requested_at = COALESCE(cancel_requested_at, now())
		WHERE id = $1 AND tenant_id = $2 AND state IN ('queued', 'running')
		RETURNING `+JobColumns, id, tenantID))
	if err == nil {
		return j, nil
	}
	if err = notFound(err); !errors.Is(err, domain.ErrNotFound) {
		return nil, err
	}
	if _, err := s.GetJob(ctx, tenantID, id); err != nil {
		return nil, err
	}
	return nil, domain.ErrConflict // exists, but already finished
}

func (s *Store) JobHistory(ctx context.Context, tenantID, id uuid.UUID) ([]domain.JobEvent, error) {
	if _, err := s.GetJob(ctx, tenantID, id); err != nil {
		return nil, err
	}
	rows, err := s.pool.Query(ctx, `
		SELECT at, from_state::text, to_state::text, attempt, worker_id, detail
		FROM job_events WHERE job_id = $1 ORDER BY id`, id)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []domain.JobEvent
	for rows.Next() {
		var e domain.JobEvent
		var from *string
		var to string
		if err := rows.Scan(&e.At, &from, &to, &e.Attempt, &e.WorkerID, &e.Detail); err != nil {
			return nil, err
		}
		if from != nil {
			fs := domain.JobState(*from)
			e.FromState = &fs
		}
		e.ToState = domain.JobState(to)
		out = append(out, e)
	}
	return out, rows.Err()
}
