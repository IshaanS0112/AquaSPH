package worker_test

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/artifacts"
	"github.com/IshaanS0112/AquaSPH/backend/internal/config"
	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/IshaanS0112/AquaSPH/backend/internal/events"
	"github.com/IshaanS0112/AquaSPH/backend/internal/ids"
	"github.com/IshaanS0112/AquaSPH/backend/internal/obs"
	"github.com/IshaanS0112/AquaSPH/backend/internal/queue"
	"github.com/IshaanS0112/AquaSPH/backend/internal/store"
	"github.com/IshaanS0112/AquaSPH/backend/internal/testutil"
	"github.com/IshaanS0112/AquaSPH/backend/internal/worker"
	"github.com/google/uuid"
	promtest "github.com/prometheus/client_golang/prometheus/testutil"
	"github.com/prometheus/client_golang/prometheus/testutil/promlint"
	"github.com/redis/go-redis/v9"
)

var bg = context.Background()

type harness struct {
	t       *testing.T
	st      *store.Store
	q       *queue.Queue
	w       *worker.Worker
	m       *obs.WorkerMetrics
	artDir  string
	tenant  *domain.Tenant
	stop    context.CancelFunc
	stopped chan struct{}
	cfg     worker.Config
	pub     events.Publisher
}

func newHarness(t *testing.T, mutate func(*worker.Config)) *harness {
	t.Helper()
	pool, url := testutil.NewDB(t)
	st := store.New(pool)
	bin, _ := filepath.Abs("../solver/testdata/fake_solver.sh")
	cfg := worker.Config{
		SolverPath: bin, Concurrency: 1, WorkDir: t.TempDir(),
		HeartbeatInterval: 100 * time.Millisecond, PollInterval: 50 * time.Millisecond,
		ReapInterval: time.Hour, DrainTimeout: 5 * time.Second, CancelGrace: 2 * time.Second,
		CacheScope: config.CacheTenant, DatabaseURL: url,
	}
	if mutate != nil {
		mutate(&cfg)
	}
	h := &harness{t: t, st: st, cfg: cfg, artDir: t.TempDir(), pub: events.Noop{},
		q: queue.New(pool, queue.Config{Lease: 2 * time.Second, RetryBase: 0, RetryMax: 0})}
	h.tenant, _ = st.CreateTenant(bg, testutil.Unique("t"), store.TenantLimits{})
	return h
}

func (h *harness) start() {
	h.t.Helper()
	art, _ := artifacts.NewLocalFS(h.artDir)
	h.m = obs.NewWorkerMetrics(obs.NewRegistry())
	w, err := worker.New(h.cfg, h.st, h.q, art, h.pub, h.m, testutil.Logger(h.t))
	if err != nil {
		h.t.Fatal(err)
	}
	h.w = w
	ctx, cancel := context.WithCancel(bg)
	h.stop, h.stopped = cancel, make(chan struct{})
	go func() { defer close(h.stopped); w.Run(ctx) }()
	h.t.Cleanup(func() { cancel(); <-h.stopped })
}

func (h *harness) enqueue(mode string, mutate func(*store.NewJob)) uuid.UUID {
	h.t.Helper()
	spec, _ := json.Marshal(map[string]any{"name": "fake", "fake_mode": mode})
	n := &store.NewJob{ID: ids.New(), TenantID: h.tenant.ID, Priority: 5, Scenario: "fake", Quality: "low",
		MaxParticles: 1000, TimeoutSeconds: 60, Spec: spec, SpecHash: mode + "-" + ids.New().String(), AllowCache: true}
	if mutate != nil {
		mutate(n)
	}
	j, err := store.InsertJob(bg, h.st.Pool(), n)
	if err != nil {
		h.t.Fatal(err)
	}
	store.NotifyWorkers(bg, h.st.Pool())
	return j.ID
}

func (h *harness) job(id uuid.UUID) *domain.Job {
	j, err := h.st.GetJobUnscoped(bg, id)
	if err != nil {
		h.t.Fatal(err)
	}
	return j
}

func (h *harness) waitFor(id uuid.UUID, what string, cond func(*domain.Job) bool) *domain.Job {
	h.t.Helper()
	deadline := time.Now().Add(15 * time.Second)
	for time.Now().Before(deadline) {
		if j := h.job(id); cond(j) {
			return j
		}
		time.Sleep(20 * time.Millisecond)
	}
	j := h.job(id)
	h.t.Fatalf("timed out waiting for %s: state=%s attempt=%d err=%v", what, j.State, j.Attempt, j.ErrorMessage)
	return nil
}

func terminal(j *domain.Job) bool { return j.State.Terminal() }

// waitMetric waits for a worker job counter: the worker updates it after the database
// write a test observes, so reading it immediately races the worker.
func (h *harness) waitMetric(result string, want float64) {
	h.t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for {
		got := promtest.ToFloat64(h.m.Jobs.WithLabelValues(result))
		if got == want {
			return
		}
		if time.Now().After(deadline) {
			h.t.Fatalf("metric %s = %v, want %v", result, got, want)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func TestCompletedJobHasResultArtifactsAndProgress(t *testing.T) {
	t.Parallel()
	h := newHarness(t, nil)
	h.start()
	id := h.enqueue("ok", nil)
	j := h.waitFor(id, "completion", terminal)
	if j.State != domain.StateCompleted || *j.Outcome != domain.OutcomeStable || j.SolverID == nil ||
		*j.SolverID != h.w.SolverID() || j.CacheKey == nil || *j.ArtifactJobID != id {
		t.Fatalf("%+v", j)
	}
	var result map[string]any
	json.Unmarshal(j.Result, &result)
	if result["status"] != "STABLE" {
		t.Fatalf("result %s", j.Result)
	}
	var arts []domain.Artifact
	json.Unmarshal(j.Artifacts, &arts)
	names := []string{}
	for _, a := range arts {
		names = append(names, a.Name)
		if _, err := os.Stat(filepath.Join(h.artDir, id.String(), a.Name)); err != nil || a.SHA256 == "" {
			t.Errorf("artifact %s: %v", a.Name, err)
		}
	}
	if strings.Join(names, ",") != "metrics.json,progress.jsonl,stderr.log,scenario.json" {
		t.Fatalf("artifacts %v", names)
	}
	var p domain.Progress
	json.Unmarshal(j.Progress, &p)
	if p.Fraction != 1 || p.Step != 20 {
		t.Fatalf("final progress %+v", p)
	}
	h.waitMetric("completed_stable", 1)
}

func TestUnstableIsACompletedResultNotAFailure(t *testing.T) {
	t.Parallel()
	h := newHarness(t, nil)
	h.start()
	j := h.waitFor(h.enqueue("unstable", nil), "completion", terminal)
	if j.State != domain.StateCompleted || *j.Outcome != domain.OutcomeUnstable || j.Attempt != 1 {
		t.Fatalf("%s %v attempt %d", j.State, j.Outcome, j.Attempt)
	}
}

func TestInvalidScenarioFailsOnceWithTheSolversMessage(t *testing.T) {
	t.Parallel()
	h := newHarness(t, nil)
	h.start()
	j := h.waitFor(h.enqueue("invalid", nil), "failure", terminal)
	if j.State != domain.StateFailed || *j.ErrorCode != domain.ErrCodeInvalidScenario || j.Attempt != 1 ||
		!strings.Contains(*j.ErrorMessage, "domain.max must exceed domain.min") {
		t.Fatalf("%s %v %v attempt %d", j.State, j.ErrorCode, j.ErrorMessage, j.Attempt)
	}
}

func TestCrashingSolverIsRetriedUpToMaxAttempts(t *testing.T) {
	t.Parallel()
	h := newHarness(t, nil)
	h.start()
	j := h.waitFor(h.enqueue("crash", nil), "final failure", terminal)
	if j.State != domain.StateFailed || *j.ErrorCode != domain.ErrCodeSolverCrashed || j.Attempt != 3 ||
		!strings.Contains(*j.ErrorMessage, "segmentation fault") {
		t.Fatalf("%s %v %v attempt %d", j.State, j.ErrorCode, j.ErrorMessage, j.Attempt)
	}
	if n := promtest.ToFloat64(h.m.Jobs.WithLabelValues("requeued")); n != 2 {
		t.Fatalf("requeued %v times, want 2", n)
	}
}

func TestUserCancelStopsTheSolverAndKeepsThePartialResult(t *testing.T) {
	t.Parallel()
	h := newHarness(t, nil)
	h.start()
	id := h.enqueue("slow", nil)
	h.waitFor(id, "progress", func(j *domain.Job) bool { return j.Progress != nil })
	if _, err := h.st.CancelJob(bg, h.tenant.ID, id); err != nil {
		t.Fatal(err)
	}
	j := h.waitFor(id, "cancellation", terminal)
	if j.State != domain.StateCancelled || !strings.Contains(string(j.Result), "CANCELLED") ||
		!strings.Contains(string(j.Artifacts), "metrics.json") {
		t.Fatalf("%s result=%s artifacts=%s", j.State, j.Result, j.Artifacts)
	}
}

func TestWallClockLimitFailsWithoutRetry(t *testing.T) {
	t.Parallel()
	h := newHarness(t, nil)
	h.start()
	id := h.enqueue("slow", func(n *store.NewJob) { n.TimeoutSeconds = 1 })
	j := h.waitFor(id, "timeout", terminal)
	if j.State != domain.StateFailed || *j.ErrorCode != domain.ErrCodeTimeout || j.Attempt != 1 ||
		!strings.Contains(string(j.Result), "CANCELLED") {
		t.Fatalf("%s %v attempt %d result %s", j.State, j.ErrorCode, j.Attempt, j.Result)
	}
}

func TestShutdownLetsShortJobsFinish(t *testing.T) {
	t.Parallel()
	h := newHarness(t, nil)
	h.start()
	id := h.enqueue("sleepy", nil)
	h.waitFor(id, "running", func(j *domain.Job) bool { return j.State == domain.StateRunning })
	h.stop()
	<-h.stopped
	if j := h.job(id); j.State != domain.StateCompleted {
		t.Fatalf("drain did not let the job finish: %s", j.State)
	}
}

func TestShutdownPastTheDrainDeadlineReleasesWithoutChargingAnAttempt(t *testing.T) {
	t.Parallel()
	h := newHarness(t, func(c *worker.Config) { c.DrainTimeout = 200 * time.Millisecond })
	h.start()
	id := h.enqueue("slow", nil)
	h.waitFor(id, "running", func(j *domain.Job) bool { return j.State == domain.StateRunning })
	h.stop()
	<-h.stopped
	j := h.job(id)
	if j.State != domain.StateQueued || j.Attempt != 0 || j.LeaseToken != nil {
		t.Fatalf("after drain timeout: %s attempt %d", j.State, j.Attempt)
	}
	var stopped int
	h.st.Pool().QueryRow(bg, `SELECT count(*) FROM workers WHERE stopped_at IS NOT NULL`).Scan(&stopped)
	if stopped != 1 {
		t.Fatal("worker did not deregister")
	}
}

func TestDuplicateQueuedBehindItsTwinIsServedFromCache(t *testing.T) {
	t.Parallel()
	h := newHarness(t, nil)
	h.start()
	same := func(n *store.NewJob) { n.SpecHash = "identical" }
	first := h.waitFor(h.enqueue("ok", same), "first run", terminal)
	second := h.waitFor(h.enqueue("ok", same), "second run", terminal)
	if !second.CacheHit || *second.SourceJobID != first.ID || *second.ArtifactJobID != first.ID ||
		string(second.Result) != string(first.Result) {
		t.Fatalf("second job not served from cache: %+v", second)
	}
	optOut := h.waitFor(h.enqueue("ok", func(n *store.NewJob) { same(n); n.AllowCache = false }), "opt-out", terminal)
	if optOut.CacheHit {
		t.Fatal("allow_cache=false was served from cache")
	}
}

// The zombie scenario end to end: this worker's lease is taken away while its solver is
// running.
func TestWorkerThatLosesItsLeaseStopsAndWritesNothing(t *testing.T) {
	t.Parallel()
	h := newHarness(t, nil)
	h.start()
	id := h.enqueue("slow", nil)
	h.waitFor(id, "running", func(j *domain.Job) bool { return j.State == domain.StateRunning && j.Progress != nil })

	h.st.Pool().Exec(bg, `UPDATE jobs SET lease_expires_at = now() - interval '1 second' WHERE id = $1`, id)
	if r, err := h.q.Reap(bg, 10); err != nil || r.Requeued != 1 {
		t.Fatalf("reap %+v %v", r, err)
	}
	// Tie up the tenant's only slot so the worker cannot reclaim it.
	h.st.Pool().Exec(bg, `UPDATE tenants SET max_concurrent_jobs = 1 WHERE id = $1`, h.tenant.ID)
	usurper, err := h.q.Claim(bg, ids.New())
	if err != nil || usurper == nil || usurper.ID != id {
		t.Fatalf("usurper claim: %v %v", usurper, err)
	}

	h.waitMetric("lease_lost", 1)
	j := h.job(id)
	if j.State != domain.StateRunning || *j.LeaseToken != *usurper.LeaseToken || j.Result != nil {
		t.Fatalf("zombie wrote over the new attempt: %s result=%s", j.State, j.Result)
	}
}

func TestProgressIsPublishedToSubscribers(t *testing.T) {
	t.Parallel()
	opts, _ := redis.ParseURL(testutil.RedisURL(t))
	rc := redis.NewClient(opts)
	defer rc.Close()
	h := newHarness(t, nil)
	h.pub = events.NewRedis(rc)
	h.start()

	var mu sync.Mutex
	var got []events.Event
	id := ids.New()
	ch, unsub := events.NewRedis(rc).Subscribe(bg, id)
	defer unsub()
	go func() {
		for e := range ch {
			mu.Lock()
			got = append(got, e)
			mu.Unlock()
		}
	}()
	h.enqueue("ok", func(n *store.NewJob) { n.ID = id })
	h.waitFor(id, "completion", terminal)
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		mu.Lock()
		n := len(got)
		last := events.Event{}
		if n > 0 {
			last = got[n-1]
		}
		mu.Unlock()
		if n >= 3 && last.Type == "state" && last.State == domain.StateCompleted {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("events: %+v", got)
}

func TestWorkerMetricsPassPromlint(t *testing.T) {
	reg := obs.NewRegistry()
	obs.NewWorkerMetrics(reg)
	obs.NewAPIMetrics(reg)
	problems, err := promlint.NewWithMetricFamilies(mustGather(t, reg)).Lint()
	if err != nil || len(problems) > 0 {
		t.Fatalf("%v %v", problems, err)
	}
}
