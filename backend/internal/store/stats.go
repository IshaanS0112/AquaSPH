package store

import (
	"context"
	"time"

	"github.com/google/uuid"
)

// Stats is a tenant's view of its own workload over a recent window, for the console dashboard.
// Workers are shared infrastructure, so only their aggregate capacity is included.
type Stats struct {
	Tenant      TenantInfo      `json:"tenant"`
	WindowHours int             `json:"window_hours"`
	States      map[string]int  `json:"states"`
	Window      WindowStats     `json:"window"`
	Hourly      []HourBucket    `json:"hourly"`
	Scenarios   []ScenarioStats `json:"scenarios"`
	Workers     WorkerCapacity  `json:"workers"`
}

// TenantInfo is the caller's own tenant and limits, so a console can show usage against them.
type TenantInfo struct {
	Name              string `json:"name"`
	MaxConcurrentJobs int    `json:"max_concurrent_jobs"`
	MaxQueuedJobs     int    `json:"max_queued_jobs"`
	MaxParticles      int    `json:"max_particles"`
	MaxWallSeconds    int    `json:"max_wall_seconds"`
	RatePerMinute     int    `json:"rate_per_minute"`
	RateBurst         int    `json:"rate_burst"`
}

type WindowStats struct {
	Submitted int `json:"submitted"`
	Completed int `json:"completed"`
	Failed    int `json:"failed"`
	Cancelled int `json:"cancelled"`
	CacheHits int `json:"cache_hits"`
	// Nil when there is nothing to measure, so a chart shows "no data" rather than zero.
	CacheHitRate   *float64 `json:"cache_hit_rate"`
	RuntimeP50     *float64 `json:"runtime_p50_s"`
	RuntimeP95     *float64 `json:"runtime_p95_s"`
	QueueWaitP95   *float64 `json:"queue_wait_p95_s"`
	StableFraction *float64 `json:"stable_fraction"`
}

type HourBucket struct {
	Hour      time.Time `json:"hour"`
	Completed int       `json:"completed"`
	CacheHits int       `json:"cache_hits"`
	Failed    int       `json:"failed"`
	Cancelled int       `json:"cancelled"`
}

type ScenarioStats struct {
	Scenario   string   `json:"scenario"`
	Runs       int      `json:"runs"`
	Stable     int      `json:"stable"`
	RuntimeP50 *float64 `json:"runtime_p50_s"`
}

type WorkerCapacity struct {
	Live  int `json:"live"`
	Slots int `json:"slots"`
	Busy  int `json:"busy"`
}

// TenantStats aggregates the last `hours` hours. Runtimes are wall time from the start of the
// final attempt to the end, over fresh runs only: a cache hit costs no solver time.
func (s *Store) TenantStats(ctx context.Context, tenantID uuid.UUID, hours int, liveWindow time.Duration) (*Stats, error) {
	window := time.Duration(hours) * time.Hour
	out := &Stats{WindowHours: hours, States: map[string]int{}}
	t := &out.Tenant
	if err := s.pool.QueryRow(ctx, `
		SELECT name, max_concurrent_jobs, max_queued_jobs, max_particles, max_wall_seconds, rate_per_minute, rate_burst
		FROM tenants WHERE id = $1`, tenantID).Scan(&t.Name, &t.MaxConcurrentJobs, &t.MaxQueuedJobs,
		&t.MaxParticles, &t.MaxWallSeconds, &t.RatePerMinute, &t.RateBurst); err != nil {
		return nil, notFound(err)
	}

	rows, err := s.pool.Query(ctx, `SELECT state::text, count(*) FROM jobs WHERE tenant_id = $1 GROUP BY state`, tenantID)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var st string
		var n int
		if err := rows.Scan(&st, &n); err != nil {
			rows.Close()
			return nil, err
		}
		out.States[st] = n
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}
	for _, st := range []string{"queued", "running", "completed", "failed", "cancelled"} {
		if _, ok := out.States[st]; !ok {
			out.States[st] = 0
		}
	}

	w := &out.Window
	err = s.pool.QueryRow(ctx, `
		WITH recent AS (
			SELECT * FROM jobs
			WHERE tenant_id = $1
			  AND (created_at > now() - make_interval(secs => $2) OR finished_at > now() - make_interval(secs => $2))
		), finished AS (
			SELECT * FROM recent WHERE finished_at > now() - make_interval(secs => $2)
		)
		SELECT
			(SELECT count(*) FROM recent WHERE created_at > now() - make_interval(secs => $2)),
			(SELECT count(*) FROM finished WHERE state = 'completed'),
			(SELECT count(*) FROM finished WHERE state = 'failed'),
			(SELECT count(*) FROM finished WHERE state = 'cancelled'),
			(SELECT count(*) FROM finished WHERE cache_hit),
			(SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY extract(epoch FROM finished_at - started_at))
			   FROM finished WHERE state = 'completed' AND NOT cache_hit AND started_at IS NOT NULL),
			(SELECT percentile_cont(0.95) WITHIN GROUP (ORDER BY extract(epoch FROM finished_at - started_at))
			   FROM finished WHERE state = 'completed' AND NOT cache_hit AND started_at IS NOT NULL),
			(SELECT percentile_cont(0.95) WITHIN GROUP (ORDER BY extract(epoch FROM started_at - created_at))
			   FROM recent WHERE started_at IS NOT NULL AND NOT cache_hit
			    AND started_at > now() - make_interval(secs => $2)),
			(SELECT count(*) FILTER (WHERE outcome = 'stable')::float8 / NULLIF(count(*), 0)
			   FROM finished WHERE state = 'completed')`,
		tenantID, window.Seconds()).Scan(&w.Submitted, &w.Completed, &w.Failed, &w.Cancelled, &w.CacheHits,
		&w.RuntimeP50, &w.RuntimeP95, &w.QueueWaitP95, &w.StableFraction)
	if err != nil {
		return nil, err
	}
	if w.Completed > 0 {
		rate := float64(w.CacheHits) / float64(w.Completed)
		w.CacheHitRate = &rate
	}

	// One bucket per hour, including empty ones, oldest first, ending with the current hour.
	end := time.Now().UTC().Truncate(time.Hour)
	buckets := make(map[time.Time]*HourBucket, hours)
	for i := hours - 1; i >= 0; i-- {
		h := end.Add(-time.Duration(i) * time.Hour)
		out.Hourly = append(out.Hourly, HourBucket{Hour: h})
	}
	for i := range out.Hourly {
		buckets[out.Hourly[i].Hour] = &out.Hourly[i]
	}
	rows, err = s.pool.Query(ctx, `
		SELECT date_trunc('hour', finished_at AT TIME ZONE 'UTC'),
		       count(*) FILTER (WHERE state = 'completed' AND NOT cache_hit),
		       count(*) FILTER (WHERE cache_hit),
		       count(*) FILTER (WHERE state = 'failed'),
		       count(*) FILTER (WHERE state = 'cancelled')
		FROM jobs
		WHERE tenant_id = $1 AND finished_at > now() - make_interval(secs => $2)
		GROUP BY 1`, tenantID, window.Seconds())
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var h time.Time
		var b HourBucket
		if err := rows.Scan(&h, &b.Completed, &b.CacheHits, &b.Failed, &b.Cancelled); err != nil {
			rows.Close()
			return nil, err
		}
		if dst, ok := buckets[time.Date(h.Year(), h.Month(), h.Day(), h.Hour(), 0, 0, 0, time.UTC)]; ok {
			dst.Completed, dst.CacheHits, dst.Failed, dst.Cancelled = b.Completed, b.CacheHits, b.Failed, b.Cancelled
		}
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}

	rows, err = s.pool.Query(ctx, `
		SELECT scenario, count(*), count(*) FILTER (WHERE outcome = 'stable'),
		       percentile_cont(0.5) WITHIN GROUP (ORDER BY extract(epoch FROM finished_at - started_at))
		           FILTER (WHERE NOT cache_hit AND started_at IS NOT NULL)
		FROM jobs
		WHERE tenant_id = $1 AND state = 'completed' AND finished_at > now() - make_interval(secs => $2)
		GROUP BY scenario ORDER BY count(*) DESC, scenario LIMIT 8`, tenantID, window.Seconds())
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var sc ScenarioStats
		if err := rows.Scan(&sc.Scenario, &sc.Runs, &sc.Stable, &sc.RuntimeP50); err != nil {
			rows.Close()
			return nil, err
		}
		out.Scenarios = append(out.Scenarios, sc)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if out.Scenarios == nil {
		out.Scenarios = []ScenarioStats{}
	}

	err = s.pool.QueryRow(ctx, `
		SELECT count(*), COALESCE(sum(concurrency), 0),
		       (SELECT count(*) FROM jobs WHERE state = 'running')
		FROM workers
		WHERE stopped_at IS NULL AND heartbeat_at > now() - make_interval(secs => $1)`,
		liveWindow.Seconds()).Scan(&out.Workers.Live, &out.Workers.Slots, &out.Workers.Busy)
	if err != nil {
		return nil, err
	}
	return out, nil
}
