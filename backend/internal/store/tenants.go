package store

import (
	"context"

	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/IshaanS0112/AquaSPH/backend/internal/ids"
	"github.com/google/uuid"
)

const tenantColumns = `id, name, max_concurrent_jobs, max_queued_jobs, max_particles,
	max_wall_seconds, rate_per_minute, rate_burst, created_at`

func scanTenant(row interface{ Scan(...any) error }) (*domain.Tenant, error) {
	var t domain.Tenant
	err := row.Scan(&t.ID, &t.Name, &t.MaxConcurrentJobs, &t.MaxQueuedJobs, &t.MaxParticles,
		&t.MaxWallSeconds, &t.RatePerMinute, &t.RateBurst, &t.CreatedAt)
	if err != nil {
		return nil, notFound(err)
	}
	return &t, nil
}

// TenantLimits holds optional limit values; nil keeps the default (on
// create) or the current value (on update).
type TenantLimits struct {
	MaxConcurrentJobs *int
	MaxQueuedJobs     *int
	MaxParticles      *int
	MaxWallSeconds    *int
	RatePerMinute     *int
	RateBurst         *int
}

func (s *Store) CreateTenant(ctx context.Context, name string, l TenantLimits) (*domain.Tenant, error) {
	return scanTenant(s.pool.QueryRow(ctx, `
		INSERT INTO tenants (id, name, max_concurrent_jobs, max_queued_jobs, max_particles,
		                     max_wall_seconds, rate_per_minute, rate_burst)
		VALUES ($1, $2, COALESCE($3, 2), COALESCE($4, 500), COALESCE($5, 400000),
		        COALESCE($6, 900), COALESCE($7, 600), COALESCE($8, 60))
		RETURNING `+tenantColumns,
		ids.New(), name, l.MaxConcurrentJobs, l.MaxQueuedJobs, l.MaxParticles,
		l.MaxWallSeconds, l.RatePerMinute, l.RateBurst))
}

func (s *Store) UpdateTenantLimits(ctx context.Context, name string, l TenantLimits) (*domain.Tenant, error) {
	return scanTenant(s.pool.QueryRow(ctx, `
		UPDATE tenants SET
			max_concurrent_jobs = COALESCE($2, max_concurrent_jobs),
			max_queued_jobs     = COALESCE($3, max_queued_jobs),
			max_particles       = COALESCE($4, max_particles),
			max_wall_seconds    = COALESCE($5, max_wall_seconds),
			rate_per_minute     = COALESCE($6, rate_per_minute),
			rate_burst          = COALESCE($7, rate_burst)
		WHERE name = $1
		RETURNING `+tenantColumns,
		name, l.MaxConcurrentJobs, l.MaxQueuedJobs, l.MaxParticles,
		l.MaxWallSeconds, l.RatePerMinute, l.RateBurst))
}

func (s *Store) GetTenant(ctx context.Context, id uuid.UUID) (*domain.Tenant, error) {
	return scanTenant(s.pool.QueryRow(ctx, `SELECT `+tenantColumns+` FROM tenants WHERE id = $1`, id))
}

func (s *Store) GetTenantByName(ctx context.Context, name string) (*domain.Tenant, error) {
	return scanTenant(s.pool.QueryRow(ctx, `SELECT `+tenantColumns+` FROM tenants WHERE name = $1`, name))
}

func (s *Store) ListTenants(ctx context.Context) ([]*domain.Tenant, error) {
	rows, err := s.pool.Query(ctx, `SELECT `+tenantColumns+` FROM tenants ORDER BY name`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []*domain.Tenant
	for rows.Next() {
		t, err := scanTenant(rows)
		if err != nil {
			return nil, err
		}
		out = append(out, t)
	}
	return out, rows.Err()
}
