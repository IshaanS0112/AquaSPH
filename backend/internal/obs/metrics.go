package obs

import (
	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/collectors"
)

// NewRegistry returns a registry with the standard Go runtime and process
// collectors. A private registry, not the global default, so tests can
// create as many as they like and nothing registers metrics by import
// side effect.
func NewRegistry() *prometheus.Registry {
	reg := prometheus.NewRegistry()
	reg.MustRegister(collectors.NewGoCollector(), collectors.NewProcessCollector(collectors.ProcessCollectorOpts{}))
	return reg
}

// Buckets for solver wall time: jobs run from well under a second (cache
// hits, tiny tests) to the default 15-minute limit.
var jobDurationBuckets = []float64{0.1, 0.5, 1, 2.5, 5, 10, 30, 60, 120, 300, 600, 900}

type WorkerMetrics struct {
	Jobs        *prometheus.CounterVec
	JobDuration *prometheus.HistogramVec
	BusySlots   prometheus.Gauge
	Slots       prometheus.Gauge
	Reaped      *prometheus.CounterVec
	ClaimErrors prometheus.Counter
}

func NewWorkerMetrics(reg prometheus.Registerer) *WorkerMetrics {
	m := &WorkerMetrics{
		Jobs: prometheus.NewCounterVec(prometheus.CounterOpts{
			Name: "aquasph_worker_jobs_total",
			Help: "Jobs finished by this worker, by result (completed, cache_hit, failed, requeued, cancelled, released, lease_lost).",
		}, []string{"result"}),
		JobDuration: prometheus.NewHistogramVec(prometheus.HistogramOpts{
			Name:    "aquasph_worker_solver_seconds",
			Help:    "Wall time of solver runs, by result.",
			Buckets: jobDurationBuckets,
		}, []string{"result"}),
		BusySlots: prometheus.NewGauge(prometheus.GaugeOpts{
			Name: "aquasph_worker_busy_slots", Help: "Jobs currently executing on this worker.",
		}),
		Slots: prometheus.NewGauge(prometheus.GaugeOpts{
			Name: "aquasph_worker_slots", Help: "Configured concurrency of this worker.",
		}),
		Reaped: prometheus.NewCounterVec(prometheus.CounterOpts{
			Name: "aquasph_reaper_jobs_total",
			Help: "Jobs whose lease expired, by what the reaper did with them.",
		}, []string{"action"}),
		ClaimErrors: prometheus.NewCounter(prometheus.CounterOpts{
			Name: "aquasph_worker_claim_errors_total", Help: "Failed claim attempts (database errors).",
		}),
	}
	reg.MustRegister(m.Jobs, m.JobDuration, m.BusySlots, m.Slots, m.Reaped, m.ClaimErrors)
	return m
}

type APIMetrics struct {
	Requests        *prometheus.CounterVec
	RequestDuration *prometheus.HistogramVec
	Submissions     *prometheus.CounterVec
	RateLimited     prometheus.Counter
	RateLimitErrors prometheus.Counter
	SSEStreams      prometheus.Gauge
}

func NewAPIMetrics(reg prometheus.Registerer) *APIMetrics {
	m := &APIMetrics{
		// route is the ServeMux pattern ("GET /v1/jobs/{id}"), never the
		// raw path: raw paths contain IDs, and one label value per job
		// would grow the metric without bound.
		Requests: prometheus.NewCounterVec(prometheus.CounterOpts{
			Name: "aquasph_http_requests_total", Help: "HTTP requests by route and status code.",
		}, []string{"route", "code"}),
		RequestDuration: prometheus.NewHistogramVec(prometheus.HistogramOpts{
			Name:    "aquasph_http_request_duration_seconds",
			Help:    "HTTP request latency by route (SSE streams excluded).",
			Buckets: []float64{.001, .0025, .005, .01, .025, .05, .1, .25, .5, 1, 2.5},
		}, []string{"route"}),
		Submissions: prometheus.NewCounterVec(prometheus.CounterOpts{
			Name: "aquasph_submissions_total",
			Help: "Job and sweep submissions by kind and result (queued, cache_hit, replayed, rejected).",
		}, []string{"kind", "result"}),
		RateLimited: prometheus.NewCounter(prometheus.CounterOpts{
			Name: "aquasph_ratelimit_rejections_total", Help: "Requests rejected with 429.",
		}),
		RateLimitErrors: prometheus.NewCounter(prometheus.CounterOpts{
			Name: "aquasph_ratelimit_errors_total",
			Help: "Rate-limit checks that failed (Redis down) and were allowed through (fail-open).",
		}),
		SSEStreams: prometheus.NewGauge(prometheus.GaugeOpts{
			Name: "aquasph_sse_streams", Help: "Open Server-Sent Event streams.",
		}),
	}
	reg.MustRegister(m.Requests, m.RequestDuration, m.Submissions, m.RateLimited, m.RateLimitErrors, m.SSEStreams)
	return m
}
