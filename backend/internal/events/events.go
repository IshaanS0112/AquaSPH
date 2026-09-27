// Package events fans job progress out to SSE subscribers through Redis
// pub/sub. It is best-effort by design: a lost message costs one progress
// update, never correctness, because the SSE handler also polls Postgres
// (the source of truth) for state changes.
package events

import (
	"context"
	"encoding/json"

	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/google/uuid"
	"github.com/redis/go-redis/v9"
)

type Event struct {
	Type     string           `json:"type"` // "progress" or "state"
	JobID    uuid.UUID        `json:"job_id"`
	State    domain.JobState  `json:"state,omitempty"`
	Progress *domain.Progress `json:"progress,omitempty"`
}

func Channel(jobID uuid.UUID) string { return "aquasph:job:" + jobID.String() }

type Publisher interface {
	Publish(ctx context.Context, e Event) error
}

// Subscriber is implemented only when Redis is configured.
type Subscriber interface {
	Subscribe(ctx context.Context, jobID uuid.UUID) (<-chan Event, func())
}

type Noop struct{}

func (Noop) Publish(context.Context, Event) error { return nil }

type Redis struct {
	client *redis.Client
}

func NewRedis(client *redis.Client) *Redis { return &Redis{client: client} }

func (r *Redis) Publish(ctx context.Context, e Event) error {
	b, err := json.Marshal(e)
	if err != nil {
		return err
	}
	return r.client.Publish(ctx, Channel(e.JobID), b).Err()
}

// Subscribe returns a channel of events for one job and a function to
// unsubscribe. The subscription is confirmed before returning, so a
// caller that subscribes and *then* reads the job's current state from
// Postgres cannot miss an event published in between.
func (r *Redis) Subscribe(ctx context.Context, jobID uuid.UUID) (<-chan Event, func()) {
	ps := r.client.Subscribe(ctx, Channel(jobID))
	out := make(chan Event, 16)
	if _, err := ps.Receive(ctx); err != nil { // wait for the subscribe confirmation
		ps.Close()
		close(out)
		return out, func() {}
	}
	done := make(chan struct{})
	go func() {
		defer close(out)
		ch := ps.Channel()
		for {
			select {
			case <-done:
				return
			case m, ok := <-ch:
				if !ok {
					return
				}
				var e Event
				if json.Unmarshal([]byte(m.Payload), &e) != nil {
					continue
				}
				select {
				case out <- e:
				default: // slow consumer: drop progress rather than block Redis
				}
			}
		}
	}()
	return out, func() { close(done); ps.Close() }
}
