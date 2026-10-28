package service

import (
	"context"
	"encoding/json"
	"fmt"
	"sort"

	"github.com/IshaanS0112/AquaSPH/backend/internal/db"
	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/IshaanS0112/AquaSPH/backend/internal/ids"
	"github.com/IshaanS0112/AquaSPH/backend/internal/scenario"
	"github.com/IshaanS0112/AquaSPH/backend/internal/store"
	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"
)

// DefaultSweepMetrics are reported by GET /v1/sweeps/{id}/results when
// the caller does not choose: pointers into the solver's metrics.json.
var DefaultSweepMetrics = []string{
	"/status", "/time/steps", "/time/wall_seconds", "/density/max",
	"/dynamics/max_speed", "/volume/final_m3",
}

type child struct {
	params    scenario.GridPoint
	spec      []byte
	specHash  string
	overrides []byte
}

// SubmitSweep validates a sweep, expands its grid and creates the sweep and all of its child
// jobs in one transaction: a sweep is never visible half-created.
func (s *Service) SubmitSweep(ctx context.Context, tenant *domain.Tenant, req SweepRequest, idemKey string) (sw *domain.Sweep, replayed bool, err error) {
	if req.Priority == nil {
		p := DefaultSweepPriority
		req.Priority = &p
	}
	b, errs := s.resolve(tenant, &req.JobRequest, DefaultSweepPriority)

	// Validate each axis value once against the resolved base (axes x
	// values checks, not the full product).
	axes := make([]string, 0, len(req.Grid))
	for p := range req.Grid {
		axes = append(axes, p)
	}
	sort.Strings(axes)
	for _, p := range axes {
		if _, dup := req.Overrides[p]; dup {
			errs = append(errs, scenario.FieldError{Field: "grid[" + p + "]",
				Message: "also set in overrides; a parameter is either fixed or swept"})
			continue
		}
		if b.spec == nil {
			continue
		}
		for i, v := range req.Grid[p] {
			probe := scenario.DeepCopy(b.spec).(map[string]any)
			if err := scenario.Override(probe, p, v); err != nil {
				errs = append(errs, scenario.FieldError{Field: fmt.Sprintf("grid[%s][%d]", p, i), Message: err.Error()})
			}
		}
	}
	points, gridErr := scenario.ExpandGrid(req.Grid, s.cfg.MaxSweepSize)
	if gridErr != nil {
		errs = append(errs, scenario.FieldError{Field: "grid", Message: gridErr.Error()})
	}
	if len(errs) > 0 {
		return nil, false, &ValidationError{Errors: errs}
	}

	children := make([]child, len(points))
	for i, pt := range points {
		spec := scenario.DeepCopy(b.spec).(map[string]any)
		merged := map[string]any{}
		for k, v := range b.overrides {
			merged[k] = v
		}
		for k, v := range pt {
			if err := scenario.Override(spec, k, v); err != nil {
				return nil, false, err // unreachable: validated above
			}
			merged[k] = v
		}
		c := child{params: pt}
		if c.spec, err = json.Marshal(spec); err != nil {
			return nil, false, err
		}
		if c.specHash, err = scenario.SpecHash(spec, b.runParams()); err != nil {
			return nil, false, err
		}
		c.overrides, _ = json.Marshal(merged)
		children[i] = c
	}

	cacheOn, scope := s.cacheScope(tenant.ID)
	keyToHash := map[string]string{}
	if cacheOn && b.allowCache {
		solvers, err := s.liveSolvers(ctx)
		if err != nil {
			return nil, false, err
		}
		for _, c := range children {
			for _, sid := range solvers {
				keyToHash[scenario.CacheKey(c.specHash, sid)] = c.specHash
			}
		}
	}
	gridJSON, _ := json.Marshal(req.Grid)
	baseOverrides, _ := json.Marshal(nonNil(b.overrides))
	reqHash := requestHash(req)

	err = db.InTx(ctx, s.store.Pool(), func(tx pgx.Tx) error {
		sweepID := ids.New()
		if idemKey != "" {
			rec, err := store.ClaimIdempotencyKey(ctx, tx, tenant.ID, idemKey, reqHash, "sweep", sweepID, 201)
			if err != nil {
				return err
			}
			if rec != nil {
				if rec.ResourceType != "sweep" || rec.RequestHash != reqHash {
					return &domain.IdempotencyMismatchError{Key: idemKey}
				}
				sw, err = s.store.GetSweep(ctx, tenant.ID, rec.ResourceID)
				replayed = true
				return err
			}
		}
		cached, err := store.FindCachedMany(ctx, tx, keyToHash, scope)
		if err != nil {
			return err
		}
		toQueue := 0
		for _, c := range children {
			if cached[c.specHash] == nil {
				toQueue++
			}
		}
		if toQueue > 0 {
			if err := checkQueueQuota(ctx, tx, tenant, toQueue); err != nil {
				return err
			}
		}
		sw, err = store.InsertSweep(ctx, tx, &domain.Sweep{
			ID: sweepID, TenantID: tenant.ID, Scenario: b.name, Quality: b.quality,
			Grid: gridJSON, BaseOverrides: baseOverrides, Total: len(children), Labels: b.labels,
		})
		if err != nil {
			return err
		}
		for _, c := range children {
			params, _ := json.Marshal(c.params)
			if _, err := store.InsertJob(ctx, tx, &store.NewJob{
				ID: ids.New(), TenantID: tenant.ID, SweepID: &sweepID, SweepParams: params,
				Priority: b.priority, Scenario: b.name, Quality: b.quality, SimTime: b.simTime,
				MaxSteps: b.maxSteps, MaxParticles: b.maxParticles, TimeoutSeconds: b.timeout,
				Spec: c.spec, Overrides: c.overrides, Labels: b.labels, SpecHash: c.specHash,
				AllowCache: b.allowCache, CacheSource: cached[c.specHash],
			}); err != nil {
				return err
			}
		}
		if toQueue > 0 {
			return store.NotifyWorkers(ctx, tx)
		}
		return nil
	})
	return sw, replayed, err
}

// SweepResultRow is one child job in a results table.
type SweepResultRow struct {
	JobID     uuid.UUID      `json:"job_id"`
	State     string         `json:"state"`
	Outcome   *string        `json:"outcome"`
	CacheHit  bool           `json:"cache_hit"`
	ErrorCode *string        `json:"error_code,omitempty"`
	Params    map[string]any `json:"params"`
	Metrics   map[string]any `json:"metrics"`
}

type SweepResults struct {
	Sweep   *domain.Sweep    `json:"sweep"`
	Axes    []string         `json:"axes"`
	Metrics []string         `json:"metric_pointers"`
	Rows    []SweepResultRow `json:"rows"`
}

// Results extracts the requested metric pointers from every child's stored result.
func (s *Service) Results(ctx context.Context, tenantID, sweepID uuid.UUID, metrics []string) (*SweepResults, error) {
	if len(metrics) == 0 {
		metrics = DefaultSweepMetrics
	}
	ptrs := make([]scenario.Pointer, len(metrics))
	var errs []scenario.FieldError
	for i, m := range metrics {
		p, err := scenario.ParsePointer(m)
		if err != nil {
			errs = append(errs, scenario.FieldError{Field: "metrics[" + m + "]", Message: err.Error()})
		}
		ptrs[i] = p
	}
	if len(errs) > 0 {
		return nil, &ValidationError{Errors: errs}
	}
	sw, err := s.store.GetSweep(ctx, tenantID, sweepID)
	if err != nil {
		return nil, err
	}
	jobs, err := s.store.SweepJobs(ctx, tenantID, sweepID)
	if err != nil {
		return nil, err
	}
	var grid map[string][]any
	_ = json.Unmarshal(sw.Grid, &grid)
	axes := make([]string, 0, len(grid))
	for a := range grid {
		axes = append(axes, a)
	}
	sort.Strings(axes)

	out := &SweepResults{Sweep: sw, Axes: axes, Metrics: metrics}
	for _, j := range jobs {
		row := SweepResultRow{JobID: j.ID, State: string(j.State), Outcome: j.Outcome, CacheHit: j.CacheHit,
			ErrorCode: j.ErrorCode, Params: map[string]any{}, Metrics: map[string]any{}}
		_ = json.Unmarshal(j.SweepParams, &row.Params)
		var result any
		if len(j.Result) > 0 {
			_ = json.Unmarshal(j.Result, &result)
		}
		for i, p := range ptrs {
			row.Metrics[metrics[i]] = nil
			if result != nil {
				if v, err := p.Get(result); err == nil {
					row.Metrics[metrics[i]] = v
				}
			}
		}
		out.Rows = append(out.Rows, row)
	}
	return out, nil
}
