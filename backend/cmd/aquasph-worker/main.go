// aquasph-worker claims jobs from the queue and runs the solver.
// Scale horizontally by running more of them; see docs/platform/runbook.md
// for sizing concurrency against solver threads.
package main

import (
	"net/http"
	"os"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/api"
	"github.com/IshaanS0112/AquaSPH/backend/internal/app"
	"github.com/IshaanS0112/AquaSPH/backend/internal/artifacts"
	"github.com/IshaanS0112/AquaSPH/backend/internal/config"
	"github.com/IshaanS0112/AquaSPH/backend/internal/db"
	"github.com/IshaanS0112/AquaSPH/backend/internal/events"
	"github.com/IshaanS0112/AquaSPH/backend/internal/obs"
	"github.com/IshaanS0112/AquaSPH/backend/internal/queue"
	"github.com/IshaanS0112/AquaSPH/backend/internal/store"
	"github.com/IshaanS0112/AquaSPH/backend/internal/worker"
)

func main() {
	cfg, err := config.LoadWorker()
	log := obs.NewLogger(os.Stderr, cfg.LogLevel, cfg.LogFormat, "aquasph-worker")
	if err != nil {
		app.Exit(log, "invalid configuration", err)
	}
	ctx, stop := app.SignalContext()
	defer stop()

	pool, err := db.Connect(ctx, cfg.DatabaseURL, int32(cfg.Concurrency)+4)
	if err != nil {
		app.Exit(log, "database", err)
	}
	defer pool.Close()
	if _, err := db.Migrate(ctx, pool); err != nil {
		app.Exit(log, "migrate", err)
	}
	art, err := artifacts.NewLocalFS(cfg.ArtifactDir)
	if err != nil {
		app.Exit(log, "artifact store", err)
	}
	var pub events.Publisher = events.Noop{}
	if rc := app.Redis(ctx, cfg.RedisURL, log); rc != nil {
		defer rc.Close()
		pub = events.NewRedis(rc)
	}

	reg := obs.NewRegistry()
	metrics := obs.NewWorkerMetrics(reg)
	q := queue.New(pool, queue.Config{Lease: cfg.LeaseDuration, RetryBase: cfg.RetryBase, RetryMax: cfg.RetryMax})
	w, err := worker.New(worker.Config{
		SolverPath: cfg.SolverPath, SolverThreads: cfg.SolverThreads, Concurrency: cfg.Concurrency,
		WorkDir: cfg.WorkDir, HeartbeatInterval: cfg.HeartbeatInterval, PollInterval: cfg.PollInterval,
		ReapInterval: cfg.ReapInterval, DrainTimeout: cfg.DrainTimeout, CancelGrace: cfg.CancelGrace,
		CacheScope: cfg.CacheScope, DatabaseURL: cfg.DatabaseURL,
	}, store.New(pool), q, art, pub, metrics, log)
	if err != nil {
		app.Exit(log, "worker", err)
	}

	mux := http.NewServeMux()
	mux.Handle("GET /metrics", app.MetricsHandler(reg))
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, _ *http.Request) { w.Write([]byte(`{"status":"ok"}`)) })
	go func() {
		if err := app.Serve(ctx, api.NewHTTPServer(cfg.InternalAddr, mux), 5*time.Second, log); err != nil {
			log.Error("metrics server", "err", err)
		}
	}()

	if err := w.Run(ctx); err != nil {
		app.Exit(log, "worker", err)
	}
}
