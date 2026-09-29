// Package config loads process configuration from the environment.
package config

import (
	"errors"
	"fmt"
	"log/slog"
	"os"
	"strconv"
	"strings"
	"time"
)

// CacheScope controls who may receive a cached result. See ADR-0002.
type CacheScope string

const (
	CacheOff    CacheScope = "off"
	CacheTenant CacheScope = "tenant"
	CacheGlobal CacheScope = "global"
)

type Common struct {
	DatabaseURL string
	// RedisURL is optional. Without it rate limiting is disabled and SSE
	// falls back to polling Postgres: degraded, not down.
	RedisURL     string
	LogLevel     slog.Level
	LogFormat    string // "json" or "text"
	InternalAddr string // /metrics, /healthz, /readyz; never exposed publicly
	ArtifactDir  string
	CacheScope   CacheScope
	RetryBase    time.Duration
	RetryMax     time.Duration
}

type API struct {
	Common
	HTTPAddr        string
	ScenarioDir     string
	MaxSweepSize    int
	MaxBodyBytes    int64
	ShutdownTimeout time.Duration
	SSEMaxDuration  time.Duration
	// LiveWorkerWindow: a worker whose last heartbeat is older than this
	// is not considered when the API computes cache keys at submit time.
	LiveWorkerWindow time.Duration
}

type Worker struct {
	Common
	SolverPath        string
	SolverThreads     int
	Concurrency       int
	WorkDir           string
	LeaseDuration     time.Duration
	HeartbeatInterval time.Duration
	PollInterval      time.Duration
	ReapInterval      time.Duration
	DrainTimeout      time.Duration
	CancelGrace       time.Duration
}

type loader struct {
	errs []error
}

func (l *loader) fail(name, format string, args ...any) {
	l.errs = append(l.errs, fmt.Errorf("%s: %s", name, fmt.Sprintf(format, args...)))
}

func (l *loader) str(name, def string) string {
	if v, ok := os.LookupEnv(name); ok && v != "" {
		return v
	}
	return def
}

func (l *loader) required(name string) string {
	v := os.Getenv(name)
	if v == "" {
		l.fail(name, "required")
	}
	return v
}

func (l *loader) int(name string, def, min, max int) int {
	raw := l.str(name, "")
	if raw == "" {
		return def
	}
	v, err := strconv.Atoi(raw)
	if err != nil {
		l.fail(name, "not an integer: %q", raw)
		return def
	}
	if v < min || v > max {
		l.fail(name, "%d outside [%d, %d]", v, min, max)
		return def
	}
	return v
}

func (l *loader) dur(name string, def, min time.Duration) time.Duration {
	raw := l.str(name, "")
	if raw == "" {
		return def
	}
	v, err := time.ParseDuration(raw)
	if err != nil {
		l.fail(name, "not a duration (e.g. 30s, 5m): %q", raw)
		return def
	}
	if v < min {
		l.fail(name, "%s is below the minimum %s", v, min)
		return def
	}
	return v
}

func (l *loader) oneOf(name, def string, allowed ...string) string {
	v := strings.ToLower(l.str(name, def))
	for _, a := range allowed {
		if v == a {
			return v
		}
	}
	l.fail(name, "%q is not one of %s", v, strings.Join(allowed, ", "))
	return def
}

func (l *loader) common() Common {
	c := Common{
		DatabaseURL:  l.required("AQUASPH_DATABASE_URL"),
		RedisURL:     l.str("AQUASPH_REDIS_URL", ""),
		LogFormat:    l.oneOf("AQUASPH_LOG_FORMAT", "json", "json", "text"),
		ArtifactDir:  l.str("AQUASPH_ARTIFACT_DIR", "./data/artifacts"),
		CacheScope:   CacheScope(l.oneOf("AQUASPH_CACHE_SCOPE", "tenant", "off", "tenant", "global")),
		RetryBase:    l.dur("AQUASPH_RETRY_BASE", 5*time.Second, 0),
		RetryMax:     l.dur("AQUASPH_RETRY_MAX", 5*time.Minute, 0),
		InternalAddr: l.str("AQUASPH_INTERNAL_ADDR", ""),
	}
	switch l.oneOf("AQUASPH_LOG_LEVEL", "info", "debug", "info", "warn", "error") {
	case "debug":
		c.LogLevel = slog.LevelDebug
	case "warn":
		c.LogLevel = slog.LevelWarn
	case "error":
		c.LogLevel = slog.LevelError
	default:
		c.LogLevel = slog.LevelInfo
	}
	if c.RetryMax < c.RetryBase {
		l.fail("AQUASPH_RETRY_MAX", "must be >= AQUASPH_RETRY_BASE")
	}
	return c
}

func LoadAPI() (API, error) {
	l := &loader{}
	c := API{
		Common:           l.common(),
		HTTPAddr:         l.str("AQUASPH_HTTP_ADDR", ":8080"),
		ScenarioDir:      l.str("AQUASPH_SCENARIO_DIR", "../configs/scenarios"),
		MaxSweepSize:     l.int("AQUASPH_MAX_SWEEP_SIZE", 256, 1, 10000),
		MaxBodyBytes:     int64(l.int("AQUASPH_MAX_BODY_BYTES", 256<<10, 1<<10, 16<<20)),
		ShutdownTimeout:  l.dur("AQUASPH_SHUTDOWN_TIMEOUT", 25*time.Second, time.Second),
		SSEMaxDuration:   l.dur("AQUASPH_SSE_MAX_DURATION", 30*time.Minute, time.Second),
		LiveWorkerWindow: l.dur("AQUASPH_LIVE_WORKER_WINDOW", time.Minute, time.Second),
	}
	if c.InternalAddr == "" {
		c.InternalAddr = ":9090"
	}
	return c, errors.Join(l.errs...)
}

func LoadWorker() (Worker, error) {
	l := &loader{}
	c := Worker{
		Common:            l.common(),
		SolverPath:        l.str("AQUASPH_SOLVER_PATH", "../build/aquasph"),
		SolverThreads:     l.int("AQUASPH_SOLVER_THREADS", 0, 0, 1024),
		Concurrency:       l.int("AQUASPH_WORKER_CONCURRENCY", 1, 1, 256),
		WorkDir:           l.str("AQUASPH_WORK_DIR", os.TempDir()),
		LeaseDuration:     l.dur("AQUASPH_LEASE_DURATION", 30*time.Second, time.Second),
		HeartbeatInterval: l.dur("AQUASPH_HEARTBEAT_INTERVAL", 5*time.Second, 100*time.Millisecond),
		PollInterval:      l.dur("AQUASPH_POLL_INTERVAL", 2*time.Second, 10*time.Millisecond),
		ReapInterval:      l.dur("AQUASPH_REAP_INTERVAL", 10*time.Second, 100*time.Millisecond),
		DrainTimeout:      l.dur("AQUASPH_DRAIN_TIMEOUT", 20*time.Second, 0),
		CancelGrace:       l.dur("AQUASPH_CANCEL_GRACE", 10*time.Second, 100*time.Millisecond),
	}
	if c.InternalAddr == "" {
		c.InternalAddr = ":9091"
	}
	// A heartbeat that is not comfortably shorter than the lease lets a
	// healthy worker lose its job to one slow round trip.
	if c.HeartbeatInterval*3 > c.LeaseDuration {
		l.fail("AQUASPH_HEARTBEAT_INTERVAL", "%s must be at most a third of AQUASPH_LEASE_DURATION (%s)",
			c.HeartbeatInterval, c.LeaseDuration)
	}
	return c, errors.Join(l.errs...)
}
