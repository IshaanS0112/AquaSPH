package webhooks

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"math/rand/v2"
	"net/http"
	"strconv"
	"sync"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
	"github.com/prometheus/client_golang/prometheus"
)

// NotifyChannel is signalled by the outbox trigger whenever it queues a delivery.
const NotifyChannel = "aquasph_webhooks"

type Config struct {
	BatchSize    int
	Concurrency  int
	PollInterval time.Duration // fallback when a notification is missed
	Timeout      time.Duration // per request
	Lease        time.Duration // how long a claimed delivery is ours
	MaxAttempts  int           // then the delivery is dead-lettered
	BackoffBase  time.Duration
	BackoffMax   time.Duration
	Retention    time.Duration // outbox events older than this are purged
	AllowPrivate bool          // development only: deliver to loopback and private addresses
}

func (c *Config) defaults() {
	if c.BatchSize == 0 {
		c.BatchSize = 32
	}
	if c.Concurrency == 0 {
		c.Concurrency = 8
	}
	if c.PollInterval == 0 {
		c.PollInterval = 5 * time.Second
	}
	if c.Timeout == 0 {
		c.Timeout = 10 * time.Second
	}
	if c.Lease == 0 {
		c.Lease = c.Timeout + 20*time.Second
	}
	if c.MaxAttempts == 0 {
		c.MaxAttempts = 8
	}
	if c.BackoffBase == 0 {
		c.BackoffBase = 10 * time.Second
	}
	if c.BackoffMax == 0 {
		c.BackoffMax = time.Hour
	}
	if c.Retention == 0 {
		c.Retention = 30 * 24 * time.Hour
	}
}

type Metrics struct {
	Deliveries *prometheus.CounterVec
	Latency    prometheus.Histogram
}

func NewMetrics(reg prometheus.Registerer) *Metrics {
	m := &Metrics{
		Deliveries: prometheus.NewCounterVec(prometheus.CounterOpts{
			Name: "aquasph_webhook_deliveries_total",
			Help: "Webhook delivery attempts by result: delivered, retry or dead.",
		}, []string{"result"}),
		Latency: prometheus.NewHistogram(prometheus.HistogramOpts{
			Name:    "aquasph_webhook_delivery_duration_seconds",
			Help:    "Time from request to response for webhook delivery attempts.",
			Buckets: []float64{0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10},
		}),
	}
	reg.MustRegister(m.Deliveries, m.Latency)
	return m
}

type Dispatcher struct {
	pool    *pgxpool.Pool
	cfg     Config
	client  *http.Client
	metrics *Metrics
	log     *slog.Logger
	now     func() time.Time
}

func NewDispatcher(pool *pgxpool.Pool, cfg Config, metrics *Metrics, log *slog.Logger) *Dispatcher {
	cfg.defaults()
	return &Dispatcher{pool: pool, cfg: cfg, client: NewHTTPClient(cfg.Timeout, cfg.AllowPrivate),
		metrics: metrics, log: log, now: time.Now}
}

// Run delivers until ctx is cancelled. wake is signalled by a LISTEN on NotifyChannel; polling
// covers missed notifications and retries coming due.
func (d *Dispatcher) Run(ctx context.Context, wake <-chan struct{}) {
	poll := time.NewTicker(d.cfg.PollInterval)
	defer poll.Stop()
	purge := time.NewTicker(time.Hour)
	defer purge.Stop()
	for {
		for {
			n, err := d.DeliverDue(ctx)
			if err != nil && ctx.Err() == nil {
				d.log.Warn("webhook dispatch failed", "err", err)
			}
			if err != nil || n < d.cfg.BatchSize {
				break
			}
		}
		select {
		case <-ctx.Done():
			return
		case <-wake:
		case <-poll.C:
		case <-purge.C:
			if n, err := d.Purge(ctx); err != nil {
				d.log.Warn("outbox purge failed", "err", err)
			} else if n > 0 {
				d.log.Info("purged outbox events", "count", n)
			}
		}
	}
}

type claimed struct {
	id        int64
	attempt   int
	url       string
	secret    string
	eventID   uuid.UUID
	eventType string
	payload   json.RawMessage
	createdAt time.Time
}

// DeliverDue claims one batch of due deliveries, sends them concurrently and records each
// outcome. It returns how many it claimed.
func (d *Dispatcher) DeliverDue(ctx context.Context) (int, error) {
	batch, err := d.claim(ctx)
	if err != nil || len(batch) == 0 {
		return 0, err
	}
	sem := make(chan struct{}, d.cfg.Concurrency)
	var wg sync.WaitGroup
	for _, c := range batch {
		wg.Add(1)
		sem <- struct{}{}
		go func() {
			defer func() { <-sem; wg.Done() }()
			d.deliver(ctx, c)
		}()
	}
	wg.Wait()
	return len(batch), nil
}

// claim takes due rows, and rows whose lease expired under a dispatcher that died mid-request,
// with SKIP LOCKED so any number of API replicas can dispatch at once.
func (d *Dispatcher) claim(ctx context.Context) ([]claimed, error) {
	rows, err := d.pool.Query(ctx, `
		WITH due AS (
			SELECT id FROM webhook_deliveries
			WHERE (state = 'pending' AND next_attempt_at <= now())
			   OR (state = 'delivering' AND lease_expires_at < now())
			ORDER BY next_attempt_at, id
			LIMIT $1
			FOR UPDATE SKIP LOCKED
		)
		UPDATE webhook_deliveries w SET state = 'delivering', attempt = w.attempt + 1,
		       lease_expires_at = now() + make_interval(secs => $2), last_attempt_at = now()
		FROM due, webhook_endpoints e, outbox_events ev
		WHERE w.id = due.id AND e.id = w.endpoint_id AND ev.id = w.event_id
		RETURNING w.id, w.attempt, e.url, e.secret, ev.id, ev.type, ev.payload, ev.created_at`,
		d.cfg.BatchSize, d.cfg.Lease.Seconds())
	if err != nil {
		return nil, err
	}
	return pgx.CollectRows(rows, func(r pgx.CollectableRow) (claimed, error) {
		var c claimed
		err := r.Scan(&c.id, &c.attempt, &c.url, &c.secret, &c.eventID, &c.eventType, &c.payload, &c.createdAt)
		return c, err
	})
}

// Envelope is the request body. Its id is stable across retries, so receivers deduplicate on it.
type Envelope struct {
	ID        uuid.UUID       `json:"id"`
	Type      string          `json:"type"`
	CreatedAt time.Time       `json:"created_at"`
	Data      json.RawMessage `json:"data"`
}

func (d *Dispatcher) deliver(ctx context.Context, c claimed) {
	data, _ := json.Marshal(map[string]json.RawMessage{dataKey(c.eventType): c.payload})
	body, _ := json.Marshal(Envelope{ID: c.eventID, Type: c.eventType, CreatedAt: c.createdAt.UTC(), Data: data})

	status, err := d.send(ctx, c, body)
	if ctx.Err() != nil {
		return // shutting down: the lease expires and another dispatcher retries
	}
	switch {
	case err == nil && status >= 200 && status < 300:
		d.record(ctx, `UPDATE webhook_deliveries SET state = 'delivered', delivered_at = now(),
			lease_expires_at = NULL, last_status = $3, last_error = NULL
			WHERE id = $1 AND attempt = $2 AND state = 'delivering'`, c.id, c.attempt, status)
		d.count("delivered")
	case c.attempt >= d.cfg.MaxAttempts:
		d.record(ctx, `UPDATE webhook_deliveries SET state = 'dead', lease_expires_at = NULL,
			last_status = $3, last_error = $4
			WHERE id = $1 AND attempt = $2 AND state = 'delivering'`, c.id, c.attempt, nullStatus(status), errText(status, err))
		d.count("dead")
		d.log.Warn("webhook dead-lettered", "delivery_id", c.id, "event_id", c.eventID, "attempts", c.attempt,
			"status", status, "err", err)
	default:
		d.record(ctx, `UPDATE webhook_deliveries SET state = 'pending', lease_expires_at = NULL,
			next_attempt_at = now() + make_interval(secs => $5), last_status = $3, last_error = $4
			WHERE id = $1 AND attempt = $2 AND state = 'delivering'`,
			c.id, c.attempt, nullStatus(status), errText(status, err), d.Backoff(c.attempt).Seconds())
		d.count("retry")
	}
}

func (d *Dispatcher) send(ctx context.Context, c claimed, body []byte) (int, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.url, bytes.NewReader(body))
	if err != nil {
		return 0, err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("User-Agent", "AquaSPH-Webhooks/1")
	req.Header.Set("AquaSPH-Event-Id", c.eventID.String())
	req.Header.Set("AquaSPH-Event-Type", c.eventType)
	req.Header.Set("AquaSPH-Delivery-Attempt", strconv.Itoa(c.attempt))
	req.Header.Set(SignatureHeader, Sign(c.secret, d.now(), body))
	start := time.Now()
	res, err := d.client.Do(req)
	if d.metrics != nil {
		d.metrics.Latency.Observe(time.Since(start).Seconds())
	}
	if err != nil {
		return 0, err
	}
	defer res.Body.Close()
	_, _ = io.Copy(io.Discard, io.LimitReader(res.Body, 64<<10))
	return res.StatusCode, nil
}

// Backoff is exponential with equal jitter: half the step is fixed, half is random, so retries
// from many failed deliveries spread out instead of arriving together.
func (d *Dispatcher) Backoff(attempt int) time.Duration {
	step := d.cfg.BackoffBase
	for i := 1; i < attempt && step < d.cfg.BackoffMax; i++ {
		step *= 2
	}
	step = min(step, d.cfg.BackoffMax)
	half := step / 2
	return half + time.Duration(rand.Int64N(int64(half)+1))
}

// Purge deletes outbox events past retention; their deliveries go with them by cascade.
func (d *Dispatcher) Purge(ctx context.Context) (int64, error) {
	tag, err := d.pool.Exec(ctx, `DELETE FROM outbox_events WHERE created_at < now() - make_interval(secs => $1)`,
		d.cfg.Retention.Seconds())
	return tag.RowsAffected(), err
}

func (d *Dispatcher) record(ctx context.Context, sql string, args ...any) {
	// The outcome is written even if ctx is cancelled mid-request, so a delivered event is not
	// sent again after a restart.
	wctx, cancel := context.WithTimeout(context.WithoutCancel(ctx), 5*time.Second)
	defer cancel()
	tag, err := d.pool.Exec(wctx, sql, args...)
	switch {
	case err != nil:
		d.log.Warn("recording webhook outcome failed", "err", err)
	case tag.RowsAffected() == 0:
		// Our lease expired and another dispatcher took the delivery; its attempt is the one
		// that counts (the attempt number is the fencing token).
		d.log.Info("webhook outcome discarded: lease lost", "delivery_id", args[0])
	}
}

func (d *Dispatcher) count(result string) {
	if d.metrics != nil {
		d.metrics.Deliveries.WithLabelValues(result).Inc()
	}
}

func dataKey(eventType string) string {
	if eventType == "webhook.test" {
		return "test"
	}
	return "job"
}

func nullStatus(status int) *int {
	if status == 0 {
		return nil
	}
	return &status
}

func errText(status int, err error) string {
	if err != nil {
		if errors.Is(err, ErrBlockedAddress) {
			return ErrBlockedAddress.Error()
		}
		return truncate(err.Error(), 300)
	}
	return fmt.Sprintf("receiver responded %d", status)
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n]
}
