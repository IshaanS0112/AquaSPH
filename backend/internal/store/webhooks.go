package store

import (
	"context"
	"encoding/json"
	"fmt"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/IshaanS0112/AquaSPH/backend/internal/ids"
	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"
)

// MaxWebhookEndpoints bounds how many endpoints a tenant can register, so one tenant cannot turn
// every job into hundreds of outbound requests.
const MaxWebhookEndpoints = 10

// WebhookEventTypes are the events a tenant can subscribe to. webhook.test is sent on request
// only, to the one endpoint being tested.
var WebhookEventTypes = []string{"job.completed", "job.failed", "job.cancelled"}

type WebhookEndpoint struct {
	ID          uuid.UUID `json:"id"`
	URL         string    `json:"url"`
	Description string    `json:"description"`
	EventTypes  []string  `json:"event_types"`
	CreatedAt   time.Time `json:"created_at"`
	// Secret is only ever returned by CreateWebhookEndpoint.
	Secret string `json:"secret,omitempty"`
}

type WebhookDelivery struct {
	ID            int64      `json:"id"`
	EventID       uuid.UUID  `json:"event_id"`
	EventType     string     `json:"event_type"`
	JobID         *uuid.UUID `json:"job_id"`
	State         string     `json:"state"`
	Attempt       int        `json:"attempt"`
	NextAttemptAt *time.Time `json:"next_attempt_at"`
	LastStatus    *int       `json:"last_status"`
	LastError     *string    `json:"last_error"`
	LastAttemptAt *time.Time `json:"last_attempt_at"`
	DeliveredAt   *time.Time `json:"delivered_at"`
	CreatedAt     time.Time  `json:"created_at"`
}

// CreateWebhookEndpoint registers an endpoint. The tenant row is locked before counting, the same
// way job admission is, so concurrent creations cannot overshoot the limit.
func (s *Store) CreateWebhookEndpoint(ctx context.Context, tenantID uuid.UUID, url, description string,
	eventTypes []string, secret string) (*WebhookEndpoint, error) {
	ep := &WebhookEndpoint{ID: ids.New(), URL: url, Description: description, EventTypes: eventTypes, Secret: secret}
	err := pgx.BeginFunc(ctx, s.pool, func(tx pgx.Tx) error {
		if _, err := tx.Exec(ctx, `SELECT 1 FROM tenants WHERE id = $1 FOR UPDATE`, tenantID); err != nil {
			return err
		}
		var n int
		if err := tx.QueryRow(ctx, `SELECT count(*) FROM webhook_endpoints WHERE tenant_id = $1`, tenantID).Scan(&n); err != nil {
			return err
		}
		if n >= MaxWebhookEndpoints {
			return &domain.QuotaError{Limit: "max_webhook_endpoints", Max: MaxWebhookEndpoints, Current: n}
		}
		return tx.QueryRow(ctx, `
			INSERT INTO webhook_endpoints (id, tenant_id, url, description, secret, event_types)
			VALUES ($1, $2, $3, $4, $5, $6) RETURNING created_at`,
			ep.ID, tenantID, url, description, secret, eventTypes).Scan(&ep.CreatedAt)
	})
	if err != nil {
		return nil, err
	}
	return ep, nil
}

func (s *Store) ListWebhookEndpoints(ctx context.Context, tenantID uuid.UUID) ([]*WebhookEndpoint, error) {
	rows, err := s.pool.Query(ctx, `
		SELECT id, url, description, event_types, created_at FROM webhook_endpoints
		WHERE tenant_id = $1 ORDER BY created_at, id`, tenantID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []*WebhookEndpoint{}
	for rows.Next() {
		var e WebhookEndpoint
		if err := rows.Scan(&e.ID, &e.URL, &e.Description, &e.EventTypes, &e.CreatedAt); err != nil {
			return nil, err
		}
		out = append(out, &e)
	}
	return out, rows.Err()
}

func (s *Store) GetWebhookEndpoint(ctx context.Context, tenantID, id uuid.UUID) (*WebhookEndpoint, error) {
	var e WebhookEndpoint
	err := s.pool.QueryRow(ctx, `
		SELECT id, url, description, event_types, created_at FROM webhook_endpoints
		WHERE tenant_id = $1 AND id = $2`, tenantID, id).Scan(&e.ID, &e.URL, &e.Description, &e.EventTypes, &e.CreatedAt)
	if err != nil {
		return nil, notFound(err)
	}
	return &e, nil
}

// DeleteWebhookEndpoint removes the endpoint and, by cascade, its pending deliveries.
func (s *Store) DeleteWebhookEndpoint(ctx context.Context, tenantID, id uuid.UUID) error {
	tag, err := s.pool.Exec(ctx, `DELETE FROM webhook_endpoints WHERE tenant_id = $1 AND id = $2`, tenantID, id)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return domain.ErrNotFound
	}
	return nil
}

// SendTestEvent queues a webhook.test event for one endpoint, so a tenant can check its receiver
// and signature verification without running a simulation.
func (s *Store) SendTestEvent(ctx context.Context, tenantID, endpointID uuid.UUID) (uuid.UUID, error) {
	var eventID uuid.UUID
	err := pgx.BeginFunc(ctx, s.pool, func(tx pgx.Tx) error {
		var exists bool
		if err := tx.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM webhook_endpoints WHERE tenant_id = $1 AND id = $2)`,
			tenantID, endpointID).Scan(&exists); err != nil {
			return err
		}
		if !exists {
			return domain.ErrNotFound
		}
		payload, _ := json.Marshal(map[string]any{"endpoint_id": endpointID, "message": "test event from AquaSPH"})
		if err := tx.QueryRow(ctx, `
			INSERT INTO outbox_events (tenant_id, type, payload) VALUES ($1, 'webhook.test', $2) RETURNING id`,
			tenantID, payload).Scan(&eventID); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, `INSERT INTO webhook_deliveries (event_id, endpoint_id) VALUES ($1, $2)`,
			eventID, endpointID); err != nil {
			return err
		}
		_, err := tx.Exec(ctx, `SELECT pg_notify('aquasph_webhooks', '')`)
		return err
	})
	return eventID, err
}

// ListWebhookDeliveries returns an endpoint's most recent deliveries, newest first.
func (s *Store) ListWebhookDeliveries(ctx context.Context, tenantID, endpointID uuid.UUID, limit int) ([]*WebhookDelivery, error) {
	if _, err := s.GetWebhookEndpoint(ctx, tenantID, endpointID); err != nil {
		return nil, err
	}
	rows, err := s.pool.Query(ctx, `
		SELECT d.id, d.event_id, ev.type, ev.job_id, d.state, d.attempt,
		       CASE WHEN d.state = 'pending' THEN d.next_attempt_at END,
		       d.last_status, d.last_error, d.last_attempt_at, d.delivered_at, d.created_at
		FROM webhook_deliveries d JOIN outbox_events ev ON ev.id = d.event_id
		WHERE d.endpoint_id = $1 ORDER BY d.id DESC LIMIT $2`, endpointID, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []*WebhookDelivery{}
	for rows.Next() {
		var d WebhookDelivery
		if err := rows.Scan(&d.ID, &d.EventID, &d.EventType, &d.JobID, &d.State, &d.Attempt, &d.NextAttemptAt,
			&d.LastStatus, &d.LastError, &d.LastAttemptAt, &d.DeliveredAt, &d.CreatedAt); err != nil {
			return nil, err
		}
		out = append(out, &d)
	}
	return out, rows.Err()
}

// RedeliverWebhook puts a dead or delivered delivery back in the queue: the manual redrive for a
// receiver that was down longer than the retry schedule.
func (s *Store) RedeliverWebhook(ctx context.Context, tenantID, endpointID uuid.UUID, deliveryID int64) error {
	tag, err := s.pool.Exec(ctx, `
		UPDATE webhook_deliveries d SET state = 'pending', attempt = 0, next_attempt_at = now(),
		       lease_expires_at = NULL
		FROM webhook_endpoints e
		WHERE d.id = $3 AND d.endpoint_id = $2 AND e.id = d.endpoint_id AND e.tenant_id = $1
		  AND d.state IN ('dead', 'delivered')`, tenantID, endpointID, deliveryID)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		var exists bool
		if err := s.pool.QueryRow(ctx, `
			SELECT EXISTS (SELECT 1 FROM webhook_deliveries d JOIN webhook_endpoints e ON e.id = d.endpoint_id
			               WHERE d.id = $3 AND d.endpoint_id = $2 AND e.tenant_id = $1)`,
			tenantID, endpointID, deliveryID).Scan(&exists); err != nil {
			return err
		}
		if !exists {
			return domain.ErrNotFound
		}
		return fmt.Errorf("%w: delivery is still in flight", domain.ErrConflict)
	}
	_, err = s.pool.Exec(ctx, `SELECT pg_notify('aquasph_webhooks', '')`)
	return err
}
