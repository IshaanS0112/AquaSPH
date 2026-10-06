// Package service implements job and sweep submission: validation, the submit-time cache
// lookup, quotas and idempotency, composed into one transaction per request.
package service

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"sort"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/config"
	"github.com/IshaanS0112/AquaSPH/backend/internal/db"
	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/IshaanS0112/AquaSPH/backend/internal/ids"
	"github.com/IshaanS0112/AquaSPH/backend/internal/scenario"
	"github.com/IshaanS0112/AquaSPH/backend/internal/store"
	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"
)

const (
	DefaultJobPriority   = 5
	DefaultSweepPriority = 3 // batch work yields to interactive jobs
	DefaultQuality       = "low"
	maxSimTime           = 300.0
	maxLabels            = 16
)

type Config struct {
	CacheScope       config.CacheScope
	LiveWorkerWindow time.Duration
	MaxSweepSize     int
}

type Service struct {
	store   *store.Store
	catalog *scenario.Catalog
	cfg     Config
}

func New(st *store.Store, cat *scenario.Catalog, cfg Config) *Service {
	return &Service{store: st, catalog: cat, cfg: cfg}
}

// ValidationError lists every problem found in a request, so a client
// fixes them in one round trip rather than one per error.
type ValidationError struct {
	Errors []scenario.FieldError
}

func (e *ValidationError) Error() string {
	if len(e.Errors) == 1 {
		return e.Errors[0].Field + ": " + e.Errors[0].Message
	}
	return fmt.Sprintf("%d validation errors", len(e.Errors))
}

// JobRequest is the body of POST /v1/jobs.
type JobRequest struct {
	Scenario       string            `json:"scenario,omitempty"`
	Spec           map[string]any    `json:"spec,omitempty"`
	Overrides      map[string]any    `json:"overrides,omitempty"`
	Quality        string            `json:"quality,omitempty"`
	SimTime        *float64          `json:"sim_time,omitempty"`
	MaxSteps       *int              `json:"max_steps,omitempty"`
	TimeoutSeconds *int              `json:"timeout_seconds,omitempty"`
	Priority       *int              `json:"priority,omitempty"`
	Labels         map[string]string `json:"labels,omitempty"`
	Cache          *bool             `json:"cache,omitempty"`
}

// SweepRequest is the body of POST /v1/sweeps: a JobRequest describing the
// base run, plus a grid of pointer -> values.
type SweepRequest struct {
	JobRequest
	Grid map[string][]any `json:"grid"`
}

// base is a validated, resolved request, before any grid is applied.
type base struct {
	name         string
	spec         map[string]any
	overrides    map[string]any
	quality      string
	simTime      *float64
	maxSteps     *int
	maxParticles int
	timeout      int
	priority     int
	labels       json.RawMessage
	allowCache   bool
}

var labelKeyRE = regexp.MustCompile(`^[a-z0-9][a-z0-9_.-]{0,62}$`)

func (s *Service) resolve(tenant *domain.Tenant, r *JobRequest, defaultPriority int) (*base, []scenario.FieldError) {
	var errs []scenario.FieldError
	bad := func(field, msg string, args ...any) {
		errs = append(errs, scenario.FieldError{Field: field, Message: fmt.Sprintf(msg, args...)})
	}
	b := &base{
		quality: DefaultQuality, priority: defaultPriority, allowCache: true,
		maxParticles: tenant.MaxParticles, timeout: tenant.MaxWallSeconds,
		simTime: r.SimTime, maxSteps: r.MaxSteps, overrides: r.Overrides,
	}

	switch {
	case r.Scenario != "" && r.Spec != nil:
		bad("spec", "give either scenario or spec, not both")
	case r.Scenario != "":
		spec, ok := s.catalog.Base(r.Scenario)
		if !ok {
			bad("scenario", "unknown scenario %q; GET /v1/scenarios lists them", r.Scenario)
		} else {
			b.name, b.spec = r.Scenario, spec
		}
	case r.Spec != nil:
		name, _ := r.Spec["name"].(string)
		if !scenario.ValidName(name) {
			bad("spec.name", "an inline spec needs a name matching %s", `^[a-z0-9][a-z0-9_-]{0,63}$`)
		} else {
			b.name, b.spec = name, scenario.DeepCopy(r.Spec).(map[string]any)
		}
	default:
		bad("scenario", "required (or give an inline spec)")
	}
	if b.spec != nil && len(r.Overrides) > 0 {
		errs = append(errs, scenario.ApplyOverrides(b.spec, r.Overrides, "overrides")...)
	}

	if r.Quality != "" {
		switch r.Quality {
		case "low", "medium", "high":
			b.quality = r.Quality
		default:
			bad("quality", "must be low, medium or high")
		}
	}
	if r.SimTime != nil && (*r.SimTime <= 0 || *r.SimTime > maxSimTime) {
		bad("sim_time", "must be in (0, %g] seconds", maxSimTime)
	}
	if r.MaxSteps != nil && (*r.MaxSteps < 1 || *r.MaxSteps > 10_000_000) {
		bad("max_steps", "must be in [1, 10000000]")
	}
	if r.TimeoutSeconds != nil {
		if *r.TimeoutSeconds < 1 || *r.TimeoutSeconds > tenant.MaxWallSeconds {
			bad("timeout_seconds", "must be in [1, %d] (your tenant's limit)", tenant.MaxWallSeconds)
		} else {
			b.timeout = *r.TimeoutSeconds
		}
	}
	if r.Priority != nil {
		if *r.Priority < 0 || *r.Priority > 9 {
			bad("priority", "must be in [0, 9]")
		} else {
			b.priority = *r.Priority
		}
	}
	if len(r.Labels) > maxLabels {
		bad("labels", "at most %d labels", maxLabels)
	}
	for k, v := range r.Labels {
		if !labelKeyRE.MatchString(k) {
			bad("labels."+k, "key must match %s", labelKeyRE)
		}
		if len(v) > 128 {
			bad("labels."+k, "value longer than 128 characters")
		}
	}
	b.labels = store.SweepLabels(r.Labels)
	if r.Cache != nil {
		b.allowCache = *r.Cache
	}
	sort.Slice(errs, func(i, j int) bool { return errs[i].Field < errs[j].Field })
	return b, errs
}

func (b *base) runParams() scenario.RunParams {
	return scenario.RunParams{Quality: b.quality, SimTime: b.simTime, MaxSteps: b.maxSteps, MaxParticles: b.maxParticles}
}

func requestHash(v any) string {
	c, _ := scenario.Canonical(v)
	sum := sha256.Sum256(c)
	return hex.EncodeToString(sum[:])
}

// cacheScope returns (enabled, tenant filter). A nil filter is global.
func (s *Service) cacheScope(tenantID uuid.UUID) (bool, *uuid.UUID) {
	switch s.cfg.CacheScope {
	case config.CacheGlobal:
		return true, nil
	case config.CacheTenant:
		return true, &tenantID
	default:
		return false, nil
	}
}

func (s *Service) liveSolvers(ctx context.Context) ([]string, error) {
	return s.store.LiveSolverIDs(ctx, s.cfg.LiveWorkerWindow)
}

func getJobTx(ctx context.Context, tx pgx.Tx, tenantID, id uuid.UUID) (*domain.Job, error) {
	return store.ScanJob(tx.QueryRow(ctx, `SELECT `+store.JobColumns+` FROM jobs WHERE id = $1 AND tenant_id = $2`,
		id, tenantID))
}

// SubmitJob validates and enqueues a job, or completes it at once from the
// cache. replayed is true when idemKey matched an earlier identical request.
func (s *Service) SubmitJob(ctx context.Context, tenant *domain.Tenant, req JobRequest, idemKey string) (job *domain.Job, replayed bool, err error) {
	b, errs := s.resolve(tenant, &req, DefaultJobPriority)
	if len(errs) > 0 {
		return nil, false, &ValidationError{Errors: errs}
	}
	specJSON, err := json.Marshal(b.spec)
	if err != nil {
		return nil, false, err
	}
	specHash, err := scenario.SpecHash(b.spec, b.runParams())
	if err != nil {
		return nil, false, err
	}
	overrides, _ := json.Marshal(nonNil(b.overrides))

	cacheOn, scope := s.cacheScope(tenant.ID)
	var cacheKeys []string
	if cacheOn && b.allowCache {
		solvers, err := s.liveSolvers(ctx)
		if err != nil {
			return nil, false, err
		}
		for _, sid := range solvers {
			cacheKeys = append(cacheKeys, scenario.CacheKey(specHash, sid))
		}
	}

	err = db.InTx(ctx, s.store.Pool(), func(tx pgx.Tx) error {
		id := ids.New()
		if idemKey != "" {
			rec, err := store.ClaimIdempotencyKey(ctx, tx, tenant.ID, idemKey, requestHash(req), "job", id, 201)
			if err != nil {
				return err
			}
			if rec != nil {
				if rec.ResourceType != "job" || rec.RequestHash != requestHash(req) {
					return &domain.IdempotencyMismatchError{Key: idemKey}
				}
				job, err = getJobTx(ctx, tx, tenant.ID, rec.ResourceID)
				replayed = true
				return err
			}
		}
		src, err := store.FindCached(ctx, tx, cacheKeys, scope)
		if err != nil {
			return err
		}
		if src == nil {
			if err := checkQueueQuota(ctx, tx, tenant, 1); err != nil {
				return err
			}
		}
		job, err = store.InsertJob(ctx, tx, &store.NewJob{
			ID: id, TenantID: tenant.ID, Priority: b.priority, Scenario: b.name, Quality: b.quality,
			SimTime: b.simTime, MaxSteps: b.maxSteps, MaxParticles: b.maxParticles, TimeoutSeconds: b.timeout,
			Spec: specJSON, Overrides: overrides, Labels: b.labels, SpecHash: specHash,
			AllowCache: b.allowCache, CacheSource: src,
		})
		if err != nil {
			return err
		}
		if src == nil {
			return store.NotifyWorkers(ctx, tx)
		}
		return nil
	})
	return job, replayed, err
}

// checkQueueQuota enforces max_queued_jobs. The advisory lock makes the
// count-then-insert atomic across concurrent submissions for one tenant.
func checkQueueQuota(ctx context.Context, tx pgx.Tx, tenant *domain.Tenant, adding int) error {
	if err := store.LockTenantQuota(ctx, tx, tenant.ID); err != nil {
		return err
	}
	n, err := store.CountQueued(ctx, tx, tenant.ID)
	if err != nil {
		return err
	}
	if n+adding > tenant.MaxQueuedJobs {
		return &domain.QuotaError{Limit: "max_queued_jobs", Max: tenant.MaxQueuedJobs, Current: n}
	}
	return nil
}

func nonNil(m map[string]any) map[string]any {
	if m == nil {
		return map[string]any{}
	}
	return m
}

// IsClientError reports whether err is the caller's fault (and should be
// a 4xx) rather than the server's.
func IsClientError(err error) bool {
	var v *ValidationError
	var q *domain.QuotaError
	var m *domain.IdempotencyMismatchError
	return errors.As(err, &v) || errors.As(err, &q) || errors.As(err, &m) ||
		errors.Is(err, domain.ErrNotFound) || errors.Is(err, domain.ErrConflict)
}
