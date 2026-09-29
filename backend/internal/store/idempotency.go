package store

import (
	"context"

	"github.com/google/uuid"
)

// IdempotencyRecord is a previously used Idempotency-Key.
type IdempotencyRecord struct {
	RequestHash  string
	ResourceType string
	ResourceID   uuid.UUID
	StatusCode   int
}

// ClaimIdempotencyKey records key for this request inside the caller's transaction and
// returns the earlier record if the key was already used.
func ClaimIdempotencyKey(ctx context.Context, q Querier, tenantID uuid.UUID, key, requestHash, resourceType string,
	resourceID uuid.UUID, status int) (*IdempotencyRecord, error) {
	tag, err := q.Exec(ctx, `
		INSERT INTO idempotency_keys (tenant_id, key, request_hash, resource_type, resource_id, status_code)
		VALUES ($1, $2, $3, $4, $5, $6)
		ON CONFLICT (tenant_id, key) DO NOTHING`,
		tenantID, key, requestHash, resourceType, resourceID, status)
	if err != nil {
		return nil, err
	}
	if tag.RowsAffected() == 1 {
		return nil, nil
	}
	var r IdempotencyRecord
	err = q.QueryRow(ctx, `SELECT request_hash, resource_type, resource_id, status_code
		FROM idempotency_keys WHERE tenant_id = $1 AND key = $2`, tenantID, key).
		Scan(&r.RequestHash, &r.ResourceType, &r.ResourceID, &r.StatusCode)
	if err != nil {
		return nil, err
	}
	return &r, nil
}
