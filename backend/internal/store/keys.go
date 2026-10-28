package store

import (
	"context"

	"github.com/IshaanS0112/AquaSPH/backend/internal/auth"
	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/IshaanS0112/AquaSPH/backend/internal/ids"
	"github.com/google/uuid"
)

// CreateAPIKey mints a key for a tenant and returns its plaintext, which
// is not recoverable afterwards.
func (s *Store) CreateAPIKey(ctx context.Context, tenantID uuid.UUID, name string) (string, *domain.APIKey, error) {
	for range 3 { // a prefix collision is ~1 in 2.8e12 per pair; retry rather than fail
		g, err := auth.Generate()
		if err != nil {
			return "", nil, err
		}
		k := domain.APIKey{ID: ids.New(), TenantID: tenantID, Prefix: g.Prefix, Name: name}
		err = s.pool.QueryRow(ctx, `
			INSERT INTO api_keys (id, tenant_id, prefix, secret_hash, name) VALUES ($1, $2, $3, $4, $5)
			RETURNING created_at`, k.ID, tenantID, g.Prefix, g.Hash, name).Scan(&k.CreatedAt)
		if IsUniqueViolation(err) {
			continue
		}
		if err != nil {
			return "", nil, err
		}
		return g.Plaintext, &k, nil
	}
	return "", nil, domain.ErrConflict
}

// LookupAPIKey returns the key and its tenant for a prefix. Revoked keys
// are returned too; the caller decides, so it can log the distinction.
func (s *Store) LookupAPIKey(ctx context.Context, prefix string) (*domain.APIKey, *domain.Tenant, error) {
	var k domain.APIKey
	var t domain.Tenant
	err := s.pool.QueryRow(ctx, `
		SELECT k.id, k.tenant_id, k.prefix, k.secret_hash, k.name, k.created_at, k.last_used_at, k.revoked_at,
		       t.id, t.name, t.max_concurrent_jobs, t.max_queued_jobs, t.max_particles,
		       t.max_wall_seconds, t.rate_per_minute, t.rate_burst, t.created_at
		FROM api_keys k JOIN tenants t ON t.id = k.tenant_id
		WHERE k.prefix = $1`, prefix).Scan(
		&k.ID, &k.TenantID, &k.Prefix, &k.SecretHash, &k.Name, &k.CreatedAt, &k.LastUsedAt, &k.RevokedAt,
		&t.ID, &t.Name, &t.MaxConcurrentJobs, &t.MaxQueuedJobs, &t.MaxParticles,
		&t.MaxWallSeconds, &t.RatePerMinute, &t.RateBurst, &t.CreatedAt)
	if err != nil {
		return nil, nil, notFound(err)
	}
	return &k, &t, nil
}

// TouchAPIKey records use at most once a minute per key, so authentication
// does not turn every read request into a write.
func (s *Store) TouchAPIKey(ctx context.Context, id uuid.UUID) error {
	_, err := s.pool.Exec(ctx, `
		UPDATE api_keys SET last_used_at = now()
		WHERE id = $1 AND (last_used_at IS NULL OR last_used_at < now() - interval '1 minute')`, id)
	return err
}

func (s *Store) RevokeAPIKey(ctx context.Context, prefix string) error {
	tag, err := s.pool.Exec(ctx, `UPDATE api_keys SET revoked_at = now() WHERE prefix = $1 AND revoked_at IS NULL`, prefix)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return domain.ErrNotFound
	}
	return nil
}

func (s *Store) ListAPIKeys(ctx context.Context, tenantID uuid.UUID) ([]*domain.APIKey, error) {
	rows, err := s.pool.Query(ctx, `
		SELECT id, tenant_id, prefix, name, created_at, last_used_at, revoked_at
		FROM api_keys WHERE tenant_id = $1 ORDER BY created_at`, tenantID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []*domain.APIKey
	for rows.Next() {
		var k domain.APIKey
		if err := rows.Scan(&k.ID, &k.TenantID, &k.Prefix, &k.Name, &k.CreatedAt, &k.LastUsedAt, &k.RevokedAt); err != nil {
			return nil, err
		}
		out = append(out, &k)
	}
	return out, rows.Err()
}
