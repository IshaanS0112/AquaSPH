package store

import (
	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/jackc/pgx/v5"
)

// JobColumns is the single column list every job query selects, paired with ScanJob.
const JobColumns = `id, tenant_id, sweep_id, sweep_params, state, outcome, priority,
	scenario, quality, sim_time, max_steps, max_particles, timeout_seconds,
	spec, overrides, labels, spec_hash, allow_cache,
	attempt, max_attempts, run_after, lease_token, lease_expires_at, worker_id,
	cancel_requested_at, progress,
	solver_id, cache_key, cache_hit, source_job_id, artifact_job_id, artifacts, result,
	error_code, error_message, created_at, started_at, finished_at, updated_at`

// ScanJob reads one row selected with JobColumns.
func ScanJob(row pgx.Row) (*domain.Job, error) {
	var j domain.Job
	var state string
	err := row.Scan(&j.ID, &j.TenantID, &j.SweepID, &j.SweepParams, &state, &j.Outcome, &j.Priority,
		&j.Scenario, &j.Quality, &j.SimTime, &j.MaxSteps, &j.MaxParticles, &j.TimeoutSeconds,
		&j.Spec, &j.Overrides, &j.Labels, &j.SpecHash, &j.AllowCache,
		&j.Attempt, &j.MaxAttempts, &j.RunAfter, &j.LeaseToken, &j.LeaseExpiresAt, &j.WorkerID,
		&j.CancelRequestedAt, &j.Progress,
		&j.SolverID, &j.CacheKey, &j.CacheHit, &j.SourceJobID, &j.ArtifactJobID, &j.Artifacts, &j.Result,
		&j.ErrorCode, &j.ErrorMessage, &j.CreatedAt, &j.StartedAt, &j.FinishedAt, &j.UpdatedAt)
	if err != nil {
		return nil, err
	}
	j.State = domain.JobState(state)
	return &j, nil
}
