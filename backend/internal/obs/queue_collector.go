package obs

import (
	"context"
	"log/slog"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/prometheus/client_golang/prometheus"
)

// QueueStats is what the collector needs from the queue.
type QueueStats interface {
	Depth(ctx context.Context) (map[domain.JobState]int64, error)
	OldestQueuedAge(ctx context.Context) (time.Duration, error)
}

// QueueCollector reports queue depth at scrape time, straight from Postgres.
type QueueCollector struct {
	q      QueueStats
	log    *slog.Logger
	jobs   *prometheus.Desc
	oldest *prometheus.Desc
}

func NewQueueCollector(q QueueStats, log *slog.Logger) *QueueCollector {
	return &QueueCollector{
		q: q, log: log,
		jobs: prometheus.NewDesc("aquasph_jobs", "Jobs currently in each state.", []string{"state"}, nil),
		oldest: prometheus.NewDesc("aquasph_queue_oldest_age_seconds",
			"How long the oldest claimable queued job has waited. The primary 'workers are not keeping up' signal.", nil, nil),
	}
}

func (c *QueueCollector) Describe(ch chan<- *prometheus.Desc) {
	ch <- c.jobs
	ch <- c.oldest
}

func (c *QueueCollector) Collect(ch chan<- prometheus.Metric) {
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	depth, err := c.q.Depth(ctx)
	if err != nil {
		c.log.Warn("queue depth scrape failed", "err", err)
		return
	}
	for _, s := range domain.AllStates {
		ch <- prometheus.MustNewConstMetric(c.jobs, prometheus.GaugeValue, float64(depth[s]), string(s))
	}
	if age, err := c.q.OldestQueuedAge(ctx); err == nil {
		ch <- prometheus.MustNewConstMetric(c.oldest, prometheus.GaugeValue, age.Seconds())
	}
}
