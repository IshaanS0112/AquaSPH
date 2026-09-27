// aquasph-api serves the public HTTP API and an internal metrics port.
// Configuration: environment variables, see internal/config.
package main

import (
	"context"
	"os"
	"sync"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/api"
	"github.com/IshaanS0112/AquaSPH/backend/internal/app"
	"github.com/IshaanS0112/AquaSPH/backend/internal/artifacts"
	"github.com/IshaanS0112/AquaSPH/backend/internal/config"
	"github.com/IshaanS0112/AquaSPH/backend/internal/db"
	"github.com/IshaanS0112/AquaSPH/backend/internal/events"
	"github.com/IshaanS0112/AquaSPH/backend/internal/obs"
	"github.com/IshaanS0112/AquaSPH/backend/internal/queue"
	"github.com/IshaanS0112/AquaSPH/backend/internal/ratelimit"
	"github.com/IshaanS0112/AquaSPH/backend/internal/scenario"
	"github.com/IshaanS0112/AquaSPH/backend/internal/service"
	"github.com/IshaanS0112/AquaSPH/backend/internal/store"
)

func main() {
	cfg, err := config.LoadAPI()
	log := obs.NewLogger(os.Stderr, cfg.LogLevel, cfg.LogFormat, "aquasph-api")
	if err != nil {
		app.Exit(log, "invalid configuration", err)
	}
	ctx, stop := app.SignalContext()
	defer stop()

	pool, err := db.Connect(ctx, cfg.DatabaseURL, 0)
	if err != nil {
		app.Exit(log, "database", err)
	}
	defer pool.Close()
	// Migrating on start is safe with any number of replicas: the
	// migrator holds an advisory lock (internal/db/migrate.go).
	if ran, err := db.Migrate(ctx, pool); err != nil {
		app.Exit(log, "migrate", err)
	} else if len(ran) > 0 {
		log.Info("applied migrations", "versions", ran)
	}
	catalog, err := scenario.LoadCatalog(cfg.ScenarioDir)
	if err != nil {
		app.Exit(log, "scenario catalogue", err)
	}
	art, err := artifacts.NewLocalFS(cfg.ArtifactDir)
	if err != nil {
		app.Exit(log, "artifact store", err)
	}

	reg := obs.NewRegistry()
	metrics := obs.NewAPIMetrics(reg)
	q := queue.New(pool, queue.Config{})
	reg.MustRegister(obs.NewQueueCollector(q, log))

	st := store.New(pool)
	deps := api.Deps{
		Store: st, Catalog: catalog, Artifacts: art, Metrics: metrics, Log: log,
		Service: service.New(st, catalog, service.Config{
			CacheScope: cfg.CacheScope, LiveWorkerWindow: cfg.LiveWorkerWindow, MaxSweepSize: cfg.MaxSweepSize,
		}),
		MaxBodyBytes: cfg.MaxBodyBytes, SSEMaxDuration: cfg.SSEMaxDuration,
	}
	rc := app.Redis(ctx, cfg.RedisURL, log)
	if rc != nil {
		defer rc.Close()
		deps.Limiter = ratelimit.NewRedis(rc, "aquasph:rl:")
		deps.Subscriber = events.NewRedis(rc)
	}
	deps.Ready = func(ctx context.Context) (map[string]string, bool) {
		checks := map[string]string{"postgres": "ok", "redis": "disabled"}
		ok := true
		if err := pool.Ping(ctx); err != nil {
			checks["postgres"], ok = err.Error(), false
		}
		if rc != nil {
			checks["redis"] = "ok"
			if err := rc.Ping(ctx).Err(); err != nil {
				checks["redis"] = "degraded: " + err.Error()
			}
		}
		return checks, ok
	}

	server := api.New(deps)
	log.Info("starting", "scenarios", len(catalog.List()), "cache_scope", cfg.CacheScope, "redis", rc != nil)
	var wg sync.WaitGroup
	errs := make(chan error, 2)
	for _, s := range []struct {
		addr string
		srv  func() error
	}{
		{cfg.HTTPAddr, func() error {
			return app.Serve(ctx, api.NewHTTPServer(cfg.HTTPAddr, server.Handler()), cfg.ShutdownTimeout, log)
		}},
		{cfg.InternalAddr, func() error {
			return app.Serve(ctx, api.NewHTTPServer(cfg.InternalAddr, server.InternalHandler(app.MetricsHandler(reg))),
				5*time.Second, log)
		}},
	} {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if err := s.srv(); err != nil {
				errs <- err
				stop()
			}
		}()
	}
	wg.Wait()
	close(errs)
	if err := <-errs; err != nil {
		app.Exit(log, "server", err)
	}
	log.Info("stopped")
}
