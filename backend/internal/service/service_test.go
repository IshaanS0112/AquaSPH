package service_test

import (
	"context"
	"encoding/json"
	"errors"
	"sync"
	"testing"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/config"
	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/IshaanS0112/AquaSPH/backend/internal/ids"
	"github.com/IshaanS0112/AquaSPH/backend/internal/queue"
	"github.com/IshaanS0112/AquaSPH/backend/internal/scenario"
	"github.com/IshaanS0112/AquaSPH/backend/internal/service"
	"github.com/IshaanS0112/AquaSPH/backend/internal/store"
	"github.com/IshaanS0112/AquaSPH/backend/internal/testutil"
	"github.com/google/uuid"
)

var ctx = context.Background()

type env struct {
	st  *store.Store
	svc *service.Service
	q   *queue.Queue
}

func newEnv(t *testing.T, scope config.CacheScope) *env {
	t.Helper()
	pool, _ := testutil.NewDB(t)
	cat, err := scenario.LoadCatalog("../../../configs/scenarios")
	if err != nil {
		t.Fatal(err)
	}
	st := store.New(pool)
	return &env{
		st:  st,
		svc: service.New(st, cat, service.Config{CacheScope: scope, LiveWorkerWindow: time.Minute, MaxSweepSize: 64}),
		q:   queue.New(pool, queue.Config{Lease: 30 * time.Second}),
	}
}

func (e *env) tenant(t *testing.T, maxQueued int) *domain.Tenant {
	t.Helper()
	tn, err := e.st.CreateTenant(ctx, testutil.Unique("t"), store.TenantLimits{MaxQueuedJobs: &maxQueued})
	if err != nil {
		t.Fatal(err)
	}
	return tn
}

func ptr[T any](v T) *T { return &v }

func damBreak() service.JobRequest {
	return service.JobRequest{Scenario: "dam_break", Quality: "low", SimTime: ptr(0.1)}
}

func TestSubmitResolvesOverridesIntoAFrozenSpec(t *testing.T) {
	t.Parallel()
	e := newEnv(t, config.CacheTenant)
	tn := e.tenant(t, 10)
	req := damBreak()
	req.Overrides = map[string]any{"/materials/0/viscosity": 1.5}
	j, replayed, err := e.svc.SubmitJob(ctx, tn, req, "")
	if err != nil || replayed {
		t.Fatal(err, replayed)
	}
	var spec map[string]any
	json.Unmarshal(j.Spec, &spec)
	if v := spec["materials"].([]any)[0].(map[string]any)["viscosity"]; v != 1.5 {
		t.Fatalf("override not in stored spec: %v", v)
	}
	if j.State != domain.StateQueued || j.Priority != 5 || j.MaxParticles != tn.MaxParticles ||
		j.TimeoutSeconds != tn.MaxWallSeconds || len(j.SpecHash) != 64 {
		t.Fatalf("unexpected job: %+v", j)
	}
}

func TestSubmitReportsEveryValidationError(t *testing.T) {
	t.Parallel()
	e := newEnv(t, config.CacheTenant)
	tn := e.tenant(t, 10)
	_, _, err := e.svc.SubmitJob(ctx, tn, service.JobRequest{
		Scenario:  "dam_break",
		Quality:   "ultra",
		SimTime:   ptr(-1.0),
		Priority:  ptr(42),
		Overrides: map[string]any{"/numerics/xsph": 0.5, "/numerics/h": "fine"},
		Labels:    map[string]string{"Bad Key": "v"},
	}, "")
	var v *service.ValidationError
	if !errors.As(err, &v) || len(v.Errors) != 6 {
		t.Fatalf("want 6 field errors, got %v", err)
	}
	if _, _, err := e.svc.SubmitJob(ctx, tn, service.JobRequest{Scenario: "nope"}, ""); !errors.As(err, &v) {
		t.Fatalf("unknown scenario: %v", err)
	}
	if _, _, err := e.svc.SubmitJob(ctx, tn, service.JobRequest{Scenario: "dam_break",
		Spec: map[string]any{"name": "x"}}, ""); !errors.As(err, &v) {
		t.Fatalf("scenario and spec together: %v", err)
	}
}

func TestInlineSpecIsAccepted(t *testing.T) {
	t.Parallel()
	e := newEnv(t, config.CacheTenant)
	tn := e.tenant(t, 10)
	j, _, err := e.svc.SubmitJob(ctx, tn, service.JobRequest{Spec: map[string]any{"name": "custom_tank", "tier": 1.0}}, "")
	if err != nil || j.Scenario != "custom_tank" {
		t.Fatalf("%v %+v", err, j)
	}
}

// 20 concurrent submissions against max_queued_jobs=5: exactly 5 win.
// Without the advisory lock, several would count 4 queued and all insert.
func TestQueueQuotaHoldsUnderConcurrentSubmission(t *testing.T) {
	t.Parallel()
	e := newEnv(t, config.CacheOff)
	tn := e.tenant(t, 5)
	var wg sync.WaitGroup
	var mu sync.Mutex
	ok, quota := 0, 0
	for i := 0; i < 20; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_, _, err := e.svc.SubmitJob(ctx, tn, damBreak(), "")
			mu.Lock()
			defer mu.Unlock()
			var q *domain.QuotaError
			switch {
			case err == nil:
				ok++
			case errors.As(err, &q):
				quota++
			default:
				t.Error(err)
			}
		}()
	}
	wg.Wait()
	if ok != 5 || quota != 15 {
		t.Fatalf("%d accepted, %d rejected; want 5 and 15", ok, quota)
	}
}

func TestIdempotentReplayReturnsTheSameJob(t *testing.T) {
	t.Parallel()
	e := newEnv(t, config.CacheOff)
	tn := e.tenant(t, 10)
	a, r1, err := e.svc.SubmitJob(ctx, tn, damBreak(), "key-1")
	if err != nil || r1 {
		t.Fatal(err)
	}
	b, r2, err := e.svc.SubmitJob(ctx, tn, damBreak(), "key-1")
	if err != nil || !r2 || a.ID != b.ID {
		t.Fatalf("replay: %v replayed=%v %s vs %s", err, r2, a.ID, b.ID)
	}
	different := damBreak()
	different.Quality = "medium"
	var m *domain.IdempotencyMismatchError
	if _, _, err := e.svc.SubmitJob(ctx, tn, different, "key-1"); !errors.As(err, &m) {
		t.Fatalf("reused key with different body: %v", err)
	}
	// Keys are per tenant: another tenant using the same key is unrelated.
	other := e.tenant(t, 10)
	c, r3, err := e.svc.SubmitJob(ctx, other, damBreak(), "key-1")
	if err != nil || r3 || c.ID == a.ID {
		t.Fatal("idempotency keys leaked across tenants")
	}
}

// Ten clients retrying one request at the same instant create one job.
func TestConcurrentRequestsWithOneIdempotencyKeyCreateOneJob(t *testing.T) {
	t.Parallel()
	e := newEnv(t, config.CacheOff)
	tn := e.tenant(t, 50)
	var wg sync.WaitGroup
	idsSeen := make(chan uuid.UUID, 10)
	for i := 0; i < 10; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			j, _, err := e.svc.SubmitJob(ctx, tn, damBreak(), "same")
			if err != nil {
				t.Error(err)
				return
			}
			idsSeen <- j.ID
		}()
	}
	wg.Wait()
	close(idsSeen)
	distinct := map[uuid.UUID]bool{}
	for id := range idsSeen {
		distinct[id] = true
	}
	var n int
	e.st.Pool().QueryRow(ctx, `SELECT count(*) FROM jobs WHERE tenant_id = $1`, tn.ID).Scan(&n)
	if len(distinct) != 1 || n != 1 {
		t.Fatalf("%d distinct job IDs returned, %d rows created", len(distinct), n)
	}
}

// completeWithSolver runs a queued job through the queue as if a worker
// with the given solver binary executed it.
func completeWithSolver(t *testing.T, e *env, solverID string) *domain.Job {
	t.Helper()
	w := ids.New()
	e.st.RegisterWorker(ctx, &domain.Worker{ID: w, Hostname: "h", PID: 1, SolverID: solverID, Version: "t", Concurrency: 1})
	j, err := e.q.Claim(ctx, w)
	if err != nil || j == nil {
		t.Fatalf("claim: %v %v", j, err)
	}
	if err := e.q.Complete(ctx, j.ID, *j.LeaseToken, queue.Completion{
		Outcome: domain.OutcomeStable, Result: json.RawMessage(`{"status":"STABLE","time":{"steps":42}}`),
		Artifacts: json.RawMessage(`[{"name":"metrics.json"}]`), SolverID: solverID,
		CacheKey: scenario.CacheKey(j.SpecHash, solverID)}); err != nil {
		t.Fatal(err)
	}
	return j
}

func TestIdenticalSubmissionIsServedFromCache(t *testing.T) {
	t.Parallel()
	e := newEnv(t, config.CacheTenant)
	tn := e.tenant(t, 10)
	e.svc.SubmitJob(ctx, tn, damBreak(), "")
	first := completeWithSolver(t, e, "solver-A")

	hit, _, err := e.svc.SubmitJob(ctx, tn, damBreak(), "")
	if err != nil {
		t.Fatal(err)
	}
	if hit.State != domain.StateCompleted || !hit.CacheHit || *hit.SourceJobID != first.ID ||
		*hit.ArtifactJobID != first.ID || string(hit.Result) == "" {
		t.Fatalf("expected a cache hit on %s, got %+v", first.ID, hit)
	}

	optOut := damBreak()
	optOut.Cache = ptr(false)
	if j, _, _ := e.svc.SubmitJob(ctx, tn, optOut, ""); j.State != domain.StateQueued {
		t.Fatal("cache:false still hit the cache")
	}
	changed := damBreak()
	changed.SimTime = ptr(0.2)
	if j, _, _ := e.svc.SubmitJob(ctx, tn, changed, ""); j.State != domain.StateQueued {
		t.Fatal("a different sim_time hit the cache")
	}
}

func TestCacheIsNotSharedAcrossTenantsUnlessGlobal(t *testing.T) {
	t.Parallel()
	for _, tc := range []struct {
		scope   config.CacheScope
		wantHit bool
	}{{config.CacheTenant, false}, {config.CacheGlobal, true}, {config.CacheOff, false}} {
		e := newEnv(t, tc.scope)
		a, b := e.tenant(t, 10), e.tenant(t, 10)
		e.svc.SubmitJob(ctx, a, damBreak(), "")
		completeWithSolver(t, e, "solver-A")
		j, _, err := e.svc.SubmitJob(ctx, b, damBreak(), "")
		if err != nil || j.CacheHit != tc.wantHit {
			t.Errorf("scope %s: cache_hit=%v, want %v (%v)", tc.scope, j.CacheHit, tc.wantHit, err)
		}
	}
}

func TestCacheIgnoresResultsFromADifferentSolverBinary(t *testing.T) {
	t.Parallel()
	e := newEnv(t, config.CacheTenant)
	tn := e.tenant(t, 10)
	e.svc.SubmitJob(ctx, tn, damBreak(), "")
	completeWithSolver(t, e, "solver-OLD")
	// The old worker goes away; only a new solver build is live.
	e.st.Pool().Exec(ctx, `UPDATE workers SET stopped_at = now() WHERE solver_id = 'solver-OLD'`)
	e.st.RegisterWorker(ctx, &domain.Worker{ID: ids.New(), Hostname: "h", PID: 2, SolverID: "solver-NEW", Version: "t", Concurrency: 1})
	if j, _, _ := e.svc.SubmitJob(ctx, tn, damBreak(), ""); j.CacheHit {
		t.Fatal("served a result computed by a solver build that is no longer deployed")
	}
}

func TestTenantsCannotSeeOrTouchEachOthersJobs(t *testing.T) {
	t.Parallel()
	e := newEnv(t, config.CacheOff)
	a, b := e.tenant(t, 10), e.tenant(t, 10)
	j, _, _ := e.svc.SubmitJob(ctx, a, damBreak(), "")
	if _, err := e.st.GetJob(ctx, b.ID, j.ID); !errors.Is(err, domain.ErrNotFound) {
		t.Errorf("get: %v", err)
	}
	if _, err := e.st.CancelJob(ctx, b.ID, j.ID); !errors.Is(err, domain.ErrNotFound) {
		t.Errorf("cancel: %v", err)
	}
	if _, err := e.st.JobHistory(ctx, b.ID, j.ID); !errors.Is(err, domain.ErrNotFound) {
		t.Errorf("history: %v", err)
	}
	if list, _, _ := e.st.ListJobs(ctx, b.ID, store.JobFilter{}, nil, 50); len(list) != 0 {
		t.Errorf("list leaked %d jobs", len(list))
	}
}

func TestCancelQueuedJobAndConflictOnFinished(t *testing.T) {
	t.Parallel()
	e := newEnv(t, config.CacheOff)
	tn := e.tenant(t, 10)
	j, _, _ := e.svc.SubmitJob(ctx, tn, damBreak(), "")
	c, err := e.st.CancelJob(ctx, tn.ID, j.ID)
	if err != nil || c.State != domain.StateCancelled || c.CancelRequestedAt == nil {
		t.Fatalf("%v %+v", err, c)
	}
	if _, err := e.st.CancelJob(ctx, tn.ID, j.ID); !errors.Is(err, domain.ErrConflict) {
		t.Fatalf("second cancel: %v", err)
	}
}

// Keyset pagination: rows inserted while a client is paging must not
// shift page boundaries (OFFSET pagination would repeat or skip rows).
func TestPaginationIsStableUnderConcurrentInserts(t *testing.T) {
	t.Parallel()
	e := newEnv(t, config.CacheOff)
	tn := e.tenant(t, 100)
	want := map[uuid.UUID]bool{}
	for i := 0; i < 25; i++ {
		j, _, _ := e.svc.SubmitJob(ctx, tn, damBreak(), "")
		want[j.ID] = true
	}
	seen := map[uuid.UUID]int{}
	var after *uuid.UUID
	pages := 0
	for {
		page, next, err := e.st.ListJobs(ctx, tn.ID, store.JobFilter{}, after, 10)
		if err != nil {
			t.Fatal(err)
		}
		pages++
		for _, j := range page {
			seen[j.ID]++
		}
		e.svc.SubmitJob(ctx, tn, damBreak(), "") // a newer job lands mid-pagination
		if next == "" {
			break
		}
		id, err := store.DecodeCursor(next)
		if err != nil {
			t.Fatal(err)
		}
		after = &id
	}
	for id := range want {
		if seen[id] != 1 {
			t.Fatalf("job %s seen %d times across %d pages", id, seen[id], pages)
		}
	}
}

func TestListFiltersByStateAndLabel(t *testing.T) {
	t.Parallel()
	e := newEnv(t, config.CacheOff)
	tn := e.tenant(t, 10)
	labelled := damBreak()
	labelled.Labels = map[string]string{"study": "viscosity"}
	e.svc.SubmitJob(ctx, tn, labelled, "")
	j, _, _ := e.svc.SubmitJob(ctx, tn, damBreak(), "")
	e.st.CancelJob(ctx, tn.ID, j.ID)

	byLabel, _, _ := e.st.ListJobs(ctx, tn.ID, store.JobFilter{Labels: map[string]string{"study": "viscosity"}}, nil, 10)
	cancelled := domain.StateCancelled
	byState, _, _ := e.st.ListJobs(ctx, tn.ID, store.JobFilter{State: &cancelled}, nil, 10)
	if len(byLabel) != 1 || len(byState) != 1 || byState[0].ID != j.ID {
		t.Fatalf("label filter %d, state filter %d", len(byLabel), len(byState))
	}
}

func TestSweepExpandsIntoChildJobsAndReportsResults(t *testing.T) {
	t.Parallel()
	e := newEnv(t, config.CacheOff)
	tn := e.tenant(t, 100)
	sw, _, err := e.svc.SubmitSweep(ctx, tn, service.SweepRequest{
		JobRequest: damBreak(),
		Grid: map[string][]any{
			"/materials/0/viscosity": {0.5, 1.0, 2.0},
			"/numerics/xsph_epsilon": {0.0, 0.5},
		},
	}, "")
	if err != nil {
		t.Fatal(err)
	}
	if sw.Total != 6 {
		t.Fatalf("total %d", sw.Total)
	}
	jobs, _ := e.st.SweepJobs(ctx, tn.ID, sw.ID)
	if len(jobs) != 6 || jobs[0].Priority != service.DefaultSweepPriority {
		t.Fatalf("%d children", len(jobs))
	}
	var spec map[string]any
	json.Unmarshal(jobs[5].Spec, &spec)
	if spec["materials"].([]any)[0].(map[string]any)["viscosity"] != 2.0 ||
		spec["numerics"].(map[string]any)["xsph_epsilon"] != 0.5 {
		t.Fatalf("last child has the wrong grid point: %s", jobs[5].SweepParams)
	}
	hashes := map[string]bool{}
	for _, j := range jobs {
		hashes[j.SpecHash] = true
	}
	if len(hashes) != 6 {
		t.Fatal("children share a spec hash")
	}

	completeWithSolver(t, e, "s") // completes the highest-priority, oldest child
	res, err := e.svc.Results(ctx, tn.ID, sw.ID, []string{"/time/steps", "/nope"})
	if err != nil {
		t.Fatal(err)
	}
	if len(res.Rows) != 6 || res.Rows[0].Metrics["/time/steps"] != 42.0 || res.Rows[0].Metrics["/nope"] != nil ||
		res.Rows[1].Metrics["/time/steps"] != nil || res.Rows[0].Params["/materials/0/viscosity"] != 0.5 {
		t.Fatalf("results: %+v", res.Rows[:2])
	}
	counts, _, _ := e.st.SweepCounts(ctx, sw.ID)
	if counts[domain.StateCompleted] != 1 || counts[domain.StateQueued] != 5 {
		t.Fatalf("counts %v", counts)
	}
	if n, _ := e.st.CancelSweep(ctx, tn.ID, sw.ID); n != 5 {
		t.Fatalf("cancelled %d, want 5", n)
	}
}

func TestSweepValidation(t *testing.T) {
	t.Parallel()
	e := newEnv(t, config.CacheOff)
	tn := e.tenant(t, 100)
	req := damBreak()
	req.Overrides = map[string]any{"/numerics/h": 0.03}
	_, _, err := e.svc.SubmitSweep(ctx, tn, service.SweepRequest{JobRequest: req, Grid: map[string][]any{
		"/numerics/h":            {0.02},        // also in overrides
		"/materials/0/viscosity": {1.0, "high"}, // second value has the wrong type
	}}, "")
	var v *service.ValidationError
	if !errors.As(err, &v) || len(v.Errors) != 2 {
		t.Fatalf("want 2 errors, got %v", err)
	}
	big := map[string][]any{"/materials/0/viscosity": make([]any, 10), "/numerics/h": make([]any, 10)}
	for i := range 10 {
		big["/materials/0/viscosity"][i] = float64(i + 1)
		big["/numerics/h"][i] = 0.01 * float64(i+1)
	}
	if _, _, err := e.svc.SubmitSweep(ctx, tn, service.SweepRequest{JobRequest: damBreak(), Grid: big}, ""); !errors.As(err, &v) {
		t.Fatalf("100 points against a limit of 64: %v", err)
	}
}

// A sweep that does not fit the queue quota is rejected whole: no sweep
// row, no partial set of children.
func TestSweepOverQuotaCreatesNothing(t *testing.T) {
	t.Parallel()
	e := newEnv(t, config.CacheOff)
	tn := e.tenant(t, 4)
	_, _, err := e.svc.SubmitSweep(ctx, tn, service.SweepRequest{JobRequest: damBreak(),
		Grid: map[string][]any{"/materials/0/viscosity": {1.0, 2.0, 3.0, 4.0, 5.0}}}, "")
	var q *domain.QuotaError
	if !errors.As(err, &q) {
		t.Fatalf("got %v", err)
	}
	var sweeps, jobs int
	e.st.Pool().QueryRow(ctx, `SELECT count(*) FROM sweeps`).Scan(&sweeps)
	e.st.Pool().QueryRow(ctx, `SELECT count(*) FROM jobs`).Scan(&jobs)
	if sweeps != 0 || jobs != 0 {
		t.Fatalf("partial sweep left behind: %d sweeps, %d jobs", sweeps, jobs)
	}
}

func TestResubmittedSweepIsServedFromCache(t *testing.T) {
	t.Parallel()
	e := newEnv(t, config.CacheTenant)
	tn := e.tenant(t, 100)
	req := service.SweepRequest{JobRequest: damBreak(), Grid: map[string][]any{"/materials/0/viscosity": {1.0, 2.0}}}
	e.svc.SubmitSweep(ctx, tn, req, "")
	completeWithSolver(t, e, "s")
	completeWithSolver(t, e, "s")
	again, _, err := e.svc.SubmitSweep(ctx, tn, req, "")
	if err != nil {
		t.Fatal(err)
	}
	counts, hits, _ := e.st.SweepCounts(ctx, again.ID)
	if counts[domain.StateCompleted] != 2 || hits != 2 {
		t.Fatalf("resubmitted sweep: %v, %d hits", counts, hits)
	}
}
