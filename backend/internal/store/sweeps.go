package store

import (
	"context"
	"encoding/json"

	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/google/uuid"
)

const sweepColumns = `id, tenant_id, scenario, quality, grid, base_overrides, total, labels, created_at`

func scanSweep(row interface{ Scan(...any) error }) (*domain.Sweep, error) {
	var s domain.Sweep
	err := row.Scan(&s.ID, &s.TenantID, &s.Scenario, &s.Quality, &s.Grid, &s.BaseOverrides, &s.Total,
		&s.Labels, &s.CreatedAt)
	if err != nil {
		return nil, notFound(err)
	}
	return &s, nil
}

func InsertSweep(ctx context.Context, q Querier, s *domain.Sweep) (*domain.Sweep, error) {
	return scanSweep(q.QueryRow(ctx, `
		INSERT INTO sweeps (id, tenant_id, scenario, quality, grid, base_overrides, total, labels)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8)
		RETURNING `+sweepColumns,
		s.ID, s.TenantID, s.Scenario, s.Quality, string(s.Grid), rawOr(s.BaseOverrides, "{}"), s.Total,
		rawOr(s.Labels, "{}")))
}

func (s *Store) GetSweep(ctx context.Context, tenantID, id uuid.UUID) (*domain.Sweep, error) {
	return scanSweep(s.pool.QueryRow(ctx, `SELECT `+sweepColumns+` FROM sweeps WHERE id = $1 AND tenant_id = $2`,
		id, tenantID))
}

// SweepCounts returns the number of child jobs in each state, plus how
// many of the completed ones were cache hits.
func (s *Store) SweepCounts(ctx context.Context, sweepID uuid.UUID) (map[domain.JobState]int, int, error) {
	counts := map[domain.JobState]int{}
	for _, st := range domain.AllStates {
		counts[st] = 0
	}
	rows, err := s.pool.Query(ctx, `SELECT state::text, count(*), count(*) FILTER (WHERE cache_hit)
		FROM jobs WHERE sweep_id = $1 GROUP BY state`, sweepID)
	if err != nil {
		return nil, 0, err
	}
	defer rows.Close()
	hits := 0
	for rows.Next() {
		var st string
		var n, h int
		if err := rows.Scan(&st, &n, &h); err != nil {
			return nil, 0, err
		}
		counts[domain.JobState(st)] = n
		hits += h
	}
	return counts, hits, rows.Err()
}

func (s *Store) ListSweeps(ctx context.Context, tenantID uuid.UUID, after *uuid.UUID, limit int) ([]*domain.Sweep, Cursor, error) {
	rows, err := s.pool.Query(ctx, `SELECT `+sweepColumns+` FROM sweeps
		WHERE tenant_id = $1 AND ($2::uuid IS NULL OR id < $2)
		ORDER BY id DESC LIMIT $3`, tenantID, after, limit+1)
	if err != nil {
		return nil, "", err
	}
	defer rows.Close()
	var out []*domain.Sweep
	for rows.Next() {
		sw, err := scanSweep(rows)
		if err != nil {
			return nil, "", err
		}
		out = append(out, sw)
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

// SweepJobs returns every child of a sweep in creation (= grid) order.
func (s *Store) SweepJobs(ctx context.Context, tenantID, sweepID uuid.UUID) ([]*domain.Job, error) {
	rows, err := s.pool.Query(ctx, `SELECT `+JobColumns+` FROM jobs
		WHERE sweep_id = $1 AND tenant_id = $2 ORDER BY id`, sweepID, tenantID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []*domain.Job
	for rows.Next() {
		j, err := ScanJob(rows)
		if err != nil {
			return nil, err
		}
		out = append(out, j)
	}
	return out, rows.Err()
}

// CancelSweep applies CancelJob's semantics to every unfinished child.
func (s *Store) CancelSweep(ctx context.Context, tenantID, sweepID uuid.UUID) (int64, error) {
	if _, err := s.GetSweep(ctx, tenantID, sweepID); err != nil {
		return 0, err
	}
	tag, err := s.pool.Exec(ctx, `
		UPDATE jobs SET
			state = CASE WHEN state = 'queued' THEN 'cancelled'::job_state ELSE state END,
			finished_at = CASE WHEN state = 'queued' THEN now() ELSE finished_at END,
			cancel_requested_at = COALESCE(cancel_requested_at, now())
		WHERE sweep_id = $1 AND tenant_id = $2 AND state IN ('queued', 'running')`, sweepID, tenantID)
	return tag.RowsAffected(), err
}

// SweepLabels is a helper for callers building child jobs.
func SweepLabels(labels map[string]string) json.RawMessage {
	if len(labels) == 0 {
		return json.RawMessage(`{}`)
	}
	b, _ := json.Marshal(labels)
	return b
}
