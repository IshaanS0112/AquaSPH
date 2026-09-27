package store

import (
	"context"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/google/uuid"
)

func (s *Store) RegisterWorker(ctx context.Context, w *domain.Worker) error {
	_, err := s.pool.Exec(ctx, `
		INSERT INTO workers (id, hostname, pid, solver_id, version, concurrency)
		VALUES ($1, $2, $3, $4, $5, $6)`, w.ID, w.Hostname, w.PID, w.SolverID, w.Version, w.Concurrency)
	return err
}

func (s *Store) WorkerHeartbeat(ctx context.Context, id uuid.UUID) error {
	_, err := s.pool.Exec(ctx, `UPDATE workers SET heartbeat_at = now() WHERE id = $1`, id)
	return err
}

func (s *Store) StopWorker(ctx context.Context, id uuid.UUID) error {
	_, err := s.pool.Exec(ctx, `UPDATE workers SET stopped_at = now() WHERE id = $1`, id)
	return err
}

// LiveSolverIDs returns the distinct solver binaries of workers that
// heartbeated within window. The API uses them to compute cache keys at
// submit time; during a rolling deploy there are two.
func (s *Store) LiveSolverIDs(ctx context.Context, window time.Duration) ([]string, error) {
	rows, err := s.pool.Query(ctx, `
		SELECT DISTINCT solver_id FROM workers
		WHERE stopped_at IS NULL AND heartbeat_at > now() - make_interval(secs => $1)`, window.Seconds())
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		out = append(out, id)
	}
	return out, rows.Err()
}

func (s *Store) ListWorkers(ctx context.Context, includeStopped bool) ([]*domain.Worker, error) {
	rows, err := s.pool.Query(ctx, `
		SELECT id, hostname, pid, solver_id, version, concurrency, started_at, heartbeat_at, stopped_at
		FROM workers WHERE $1 OR stopped_at IS NULL ORDER BY started_at DESC`, includeStopped)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []*domain.Worker
	for rows.Next() {
		var w domain.Worker
		if err := rows.Scan(&w.ID, &w.Hostname, &w.PID, &w.SolverID, &w.Version, &w.Concurrency,
			&w.StartedAt, &w.HeartbeatAt, &w.StoppedAt); err != nil {
			return nil, err
		}
		out = append(out, &w)
	}
	return out, rows.Err()
}
