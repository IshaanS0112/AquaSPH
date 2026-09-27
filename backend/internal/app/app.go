// Package app holds start-up plumbing shared by the binaries: signal
// handling, Redis connection, and serving the internal metrics port.
package app

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"github.com/redis/go-redis/v9"
)

// SignalContext is cancelled on the first SIGINT or SIGTERM. A second
// signal is left to its default action (terminate), so an operator can
// always force-quit a process stuck in shutdown.
func SignalContext() (context.Context, context.CancelFunc) {
	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-ctx.Done()
		signal.Reset(syscall.SIGINT, syscall.SIGTERM)
	}()
	return ctx, cancel
}

// Redis connects if url is non-empty. A configured but unreachable Redis
// is logged, not fatal: every Redis-backed feature degrades gracefully.
func Redis(ctx context.Context, url string, log *slog.Logger) *redis.Client {
	if url == "" {
		log.Warn("AQUASPH_REDIS_URL not set: rate limiting disabled, SSE will poll Postgres")
		return nil
	}
	opts, err := redis.ParseURL(url)
	if err != nil {
		log.Error("invalid AQUASPH_REDIS_URL; continuing without Redis", "err", err)
		return nil
	}
	c := redis.NewClient(opts)
	pctx, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	if err := c.Ping(pctx).Err(); err != nil {
		log.Warn("redis unreachable at start-up; features will recover when it is", "err", err)
	}
	return c
}

// MetricsHandler exposes a registry in the Prometheus text format.
func MetricsHandler(reg *prometheus.Registry) http.Handler {
	return promhttp.HandlerFor(reg, promhttp.HandlerOpts{Registry: reg})
}

// Listen binds addr now, so a port conflict fails start-up loudly
// instead of leaving a process running without its metrics endpoint
// (invisible to monitoring), which is what happened before this existed.
func Listen(addr string) (net.Listener, error) {
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return nil, fmt.Errorf("listen on %s: %w", addr, err)
	}
	return ln, nil
}

// Serve runs srv on ln until ctx is cancelled, then shuts it down
// gracefully within timeout. It returns once the server has stopped.
func Serve(ctx context.Context, srv *http.Server, ln net.Listener, timeout time.Duration, log *slog.Logger) error {
	errc := make(chan error, 1)
	go func() {
		log.Info("listening", "addr", ln.Addr().String())
		if err := srv.Serve(ln); err != nil && !errors.Is(err, http.ErrServerClosed) {
			errc <- err
		}
		close(errc)
	}()
	select {
	case err := <-errc:
		return err
	case <-ctx.Done():
	}
	sctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	log.Info("shutting down", "addr", srv.Addr)
	return srv.Shutdown(sctx)
}

func Exit(log *slog.Logger, msg string, err error) {
	log.Error(msg, "err", err)
	os.Exit(1)
}
