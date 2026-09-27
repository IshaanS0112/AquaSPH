// Package api is the HTTP interface: routing, authentication, rate
// limiting, request/response encoding and SSE. Business rules live in
// package service; this package translates.
package api

import (
	"context"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"sync"
	"time"

	apispec "github.com/IshaanS0112/AquaSPH/backend/api"
	"github.com/IshaanS0112/AquaSPH/backend/internal/artifacts"
	"github.com/IshaanS0112/AquaSPH/backend/internal/events"
	"github.com/IshaanS0112/AquaSPH/backend/internal/obs"
	"github.com/IshaanS0112/AquaSPH/backend/internal/ratelimit"
	"github.com/IshaanS0112/AquaSPH/backend/internal/scenario"
	"github.com/IshaanS0112/AquaSPH/backend/internal/service"
	"github.com/IshaanS0112/AquaSPH/backend/internal/store"
)

type Deps struct {
	Store     *store.Store
	Service   *service.Service
	Catalog   *scenario.Catalog
	Artifacts artifacts.Store
	// Limiter is nil when Redis is not configured (rate limiting off).
	Limiter ratelimit.Limiter
	// Subscriber is nil when Redis is not configured (SSE polls Postgres).
	Subscriber events.Subscriber
	Metrics    *obs.APIMetrics
	Log        *slog.Logger
	// Ready reports dependency health for /readyz.
	Ready func(ctx context.Context) (map[string]string, bool)

	MaxBodyBytes    int64
	SSEMaxDuration  time.Duration
	SSEPollInterval time.Duration
	SSEPingInterval time.Duration
}

type Server struct {
	store        *store.Store
	svc          *service.Service
	catalog      *scenario.Catalog
	artifacts    artifacts.Store
	limiter      ratelimit.Limiter
	subscriber   events.Subscriber
	metrics      *obs.APIMetrics
	log          *slog.Logger
	ready        func(ctx context.Context) (map[string]string, bool)
	maxBodyBytes int64
	sseMax       time.Duration
	ssePoll      time.Duration
	ssePing      time.Duration
	touched      sync.Map // api key ID -> time of last last_used_at write
	mux          *http.ServeMux
}

func New(d Deps) *Server {
	s := &Server{
		store: d.Store, svc: d.Service, catalog: d.Catalog, artifacts: d.Artifacts, limiter: d.Limiter,
		subscriber: d.Subscriber, metrics: d.Metrics, log: d.Log, ready: d.Ready,
		maxBodyBytes: d.MaxBodyBytes, sseMax: d.SSEMaxDuration, ssePoll: d.SSEPollInterval, ssePing: d.SSEPingInterval,
	}
	if s.maxBodyBytes == 0 {
		s.maxBodyBytes = 256 << 10
	}
	if s.sseMax == 0 {
		s.sseMax = 30 * time.Minute
	}
	if s.ssePoll == 0 {
		s.ssePoll = time.Second
	}
	if s.ssePing == 0 {
		s.ssePing = 15 * time.Second
	}
	s.mux = http.NewServeMux()
	for _, rt := range s.routes() {
		h := rt.handler
		if rt.auth {
			h = s.authenticate(h)
		}
		s.mux.HandleFunc(rt.pattern, h)
	}
	return s
}

type route struct {
	pattern string
	auth    bool
	handler http.HandlerFunc
}

// routes is the complete public surface. TestEveryRouteIsDocumented
// checks each pattern against api/openapi.yaml.
func (s *Server) routes() []route {
	return []route{
		{"GET /healthz", false, s.healthz},
		{"GET /readyz", false, s.readyz},
		{"GET /openapi.yaml", false, s.openapi},

		{"GET /v1/scenarios", true, s.listScenarios},
		{"GET /v1/scenarios/{name}", true, s.getScenario},

		{"POST /v1/jobs", true, s.createJob},
		{"GET /v1/jobs", true, s.listJobs},
		{"GET /v1/jobs/{id}", true, s.getJob},
		{"POST /v1/jobs/{id}/cancel", true, s.cancelJob},
		{"GET /v1/jobs/{id}/events", true, s.jobEvents},
		{"GET /v1/jobs/{id}/history", true, s.jobHistory},
		{"GET /v1/jobs/{id}/result", true, s.jobResult},
		{"GET /v1/jobs/{id}/artifacts", true, s.listArtifacts},
		{"GET /v1/jobs/{id}/artifacts/{name}", true, s.getArtifact},

		{"POST /v1/sweeps", true, s.createSweep},
		{"GET /v1/sweeps", true, s.listSweeps},
		{"GET /v1/sweeps/{id}", true, s.getSweep},
		{"GET /v1/sweeps/{id}/results", true, s.sweepResults},
		{"POST /v1/sweeps/{id}/cancel", true, s.cancelSweep},
	}
}

// Handler is the public API.
func (s *Server) Handler() http.Handler {
	return s.observe(http.HandlerFunc(s.dispatch))
}

// dispatch routes through the mux, but turns the mux's plain-text 404
// and 405 responses into problem+json like every other error.
func (s *Server) dispatch(w http.ResponseWriter, r *http.Request) {
	if _, pattern := s.mux.Handler(r); pattern != "" {
		s.mux.ServeHTTP(w, r)
		return
	}
	rec := httptest.NewRecorder()
	s.mux.ServeHTTP(rec, r)
	if allow := rec.Header().Get("Allow"); allow != "" {
		w.Header().Set("Allow", allow)
	}
	writeProblem(w, r, Problem{Type: problemType("no-route"), Status: rec.Code,
		Detail: "no route for " + r.Method + " " + r.URL.Path})
}

// InternalHandler serves /metrics and probes on the internal port.
func (s *Server) InternalHandler(metrics http.Handler) http.Handler {
	mux := http.NewServeMux()
	mux.Handle("GET /metrics", metrics)
	mux.HandleFunc("GET /healthz", s.healthz)
	mux.HandleFunc("GET /readyz", s.readyz)
	return mux
}

// NewHTTPServer applies timeouts that protect against slow clients.
// WriteTimeout is lifted per-request by the SSE handler, which is the
// only long-lived response.
func NewHTTPServer(addr string, h http.Handler) *http.Server {
	return &http.Server{
		Addr:              addr,
		Handler:           h,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       30 * time.Second,
		WriteTimeout:      30 * time.Second,
		IdleTimeout:       120 * time.Second,
		MaxHeaderBytes:    64 << 10,
	}
}

func (s *Server) healthz(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

// readyz: ready means Postgres is reachable. Redis is reported but does
// not gate readiness, because the API works (degraded) without it.
func (s *Server) readyz(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
	defer cancel()
	checks, ok := s.ready(ctx)
	status, code := "ready", http.StatusOK
	if !ok {
		status, code = "unavailable", http.StatusServiceUnavailable
	}
	writeJSON(w, code, map[string]any{"status": status, "checks": checks})
}

func (s *Server) openapi(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/yaml")
	w.Header().Set("Cache-Control", "public, max-age=300")
	_, _ = w.Write(apispec.OpenAPI)
}
