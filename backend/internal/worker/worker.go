// Package worker claims jobs from the queue and runs them through the
// solver: cache check, workdir, solver supervision with heartbeats,
// artifact upload, and a fenced write of the outcome.
package worker

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"runtime/debug"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/artifacts"
	"github.com/IshaanS0112/AquaSPH/backend/internal/config"
	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/IshaanS0112/AquaSPH/backend/internal/events"
	"github.com/IshaanS0112/AquaSPH/backend/internal/ids"
	"github.com/IshaanS0112/AquaSPH/backend/internal/obs"
	"github.com/IshaanS0112/AquaSPH/backend/internal/queue"
	"github.com/IshaanS0112/AquaSPH/backend/internal/scenario"
	"github.com/IshaanS0112/AquaSPH/backend/internal/solver"
	"github.com/IshaanS0112/AquaSPH/backend/internal/store"
	"github.com/google/uuid"
)

// Why a run was stopped, carried as the context cause so the supervisor's
// Outcome says which of these it was.
var (
	errUserCancel = errors.New("cancellation requested")
	errTimeout    = errors.New("wall-clock limit reached")
	errShutdown   = errors.New("worker shutting down")
	errLeaseLost  = errors.New("lease lost")
)

type Config struct {
	SolverPath        string
	SolverThreads     int
	Concurrency       int
	WorkDir           string
	HeartbeatInterval time.Duration
	PollInterval      time.Duration
	ReapInterval      time.Duration
	DrainTimeout      time.Duration
	CancelGrace       time.Duration
	CacheScope        config.CacheScope
	// DatabaseURL enables LISTEN/NOTIFY wake-ups. Empty means poll only.
	DatabaseURL string
}

type Worker struct {
	id        uuid.UUID
	cfg       Config
	store     *store.Store
	queue     *queue.Queue
	artifacts artifacts.Store
	events    events.Publisher
	metrics   *obs.WorkerMetrics
	log       *slog.Logger
	solverID  string
	wake      chan struct{}
}

func New(cfg Config, st *store.Store, q *queue.Queue, art artifacts.Store, pub events.Publisher,
	m *obs.WorkerMetrics, log *slog.Logger) (*Worker, error) {
	sid, err := solver.Identity(cfg.SolverPath)
	if err != nil {
		return nil, fmt.Errorf("hash solver binary %s: %w", cfg.SolverPath, err)
	}
	if err := os.MkdirAll(cfg.WorkDir, 0o750); err != nil {
		return nil, err
	}
	id := ids.New()
	return &Worker{
		id: id, cfg: cfg, store: st, queue: q, artifacts: art, events: pub, metrics: m, solverID: sid,
		log:  log.With("worker_id", id),
		wake: make(chan struct{}, 1),
	}, nil
}

func (w *Worker) ID() uuid.UUID    { return w.id }
func (w *Worker) SolverID() string { return w.solverID }

// Run claims and executes jobs until ctx is cancelled, then drains: it
// stops claiming, gives running jobs DrainTimeout to finish, and hands
// back (without charging an attempt) any that do not.
func (w *Worker) Run(ctx context.Context) error {
	host, _ := os.Hostname()
	if err := w.store.RegisterWorker(ctx, &domain.Worker{
		ID: w.id, Hostname: host, PID: os.Getpid(), SolverID: w.solverID,
		Version: buildVersion(), Concurrency: w.cfg.Concurrency,
	}); err != nil {
		return fmt.Errorf("register worker: %w", err)
	}
	w.log.Info("worker started", "solver_id", w.solverID[:12], "concurrency", w.cfg.Concurrency,
		"solver", w.cfg.SolverPath)
	w.metrics.Slots.Set(float64(w.cfg.Concurrency))

	bg, stopBG := context.WithCancel(context.Background())
	defer stopBG()
	if w.cfg.DatabaseURL != "" {
		go queue.Listen(bg, w.cfg.DatabaseURL, w.wake, w.log)
	}
	go w.every(bg, w.cfg.ReapInterval, w.reap)
	go w.every(bg, 10*time.Second, func(ctx context.Context) {
		if err := w.store.WorkerHeartbeat(ctx, w.id); err != nil {
			w.log.Warn("registry heartbeat failed", "err", err)
		}
	})

	// Jobs run under their own context, not ctx: a SIGTERM to the worker
	// should first let them finish (drain), and only cancel them when the
	// drain deadline passes.
	jobsCtx, cancelJobs := context.WithCancelCause(context.Background())
	defer cancelJobs(nil)
	var running sync.WaitGroup
	slots := make(chan struct{}, w.cfg.Concurrency)
	poll := time.NewTicker(w.cfg.PollInterval)
	defer poll.Stop()

claimLoop:
	for {
		select {
		case slots <- struct{}{}:
		case <-ctx.Done():
			break claimLoop
		}
		job, err := w.queue.Claim(ctx, w.id)
		if err != nil || job == nil {
			<-slots
			if err != nil && ctx.Err() == nil {
				w.metrics.ClaimErrors.Inc()
				w.log.Error("claim failed", "err", err)
			}
			select {
			case <-ctx.Done():
				break claimLoop
			case <-w.wake:
			case <-poll.C:
			}
			continue
		}
		running.Add(1)
		w.metrics.BusySlots.Inc()
		go func() {
			defer func() {
				w.metrics.BusySlots.Dec()
				<-slots
				running.Done()
				w.nudge() // a slot freed up; look for more work now rather than at the next poll
			}()
			w.execute(jobsCtx, job)
		}()
	}

	w.log.Info("draining", "timeout", w.cfg.DrainTimeout)
	drained := make(chan struct{})
	go func() { running.Wait(); close(drained) }()
	select {
	case <-drained:
	case <-time.After(w.cfg.DrainTimeout):
		w.log.Warn("drain timeout; releasing running jobs back to the queue")
		cancelJobs(errShutdown)
		<-drained
	}
	stopCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = w.store.StopWorker(stopCtx, w.id)
	w.log.Info("worker stopped")
	return nil
}

func (w *Worker) nudge() {
	select {
	case w.wake <- struct{}{}:
	default:
	}
}

func (w *Worker) every(ctx context.Context, d time.Duration, fn func(context.Context)) {
	t := time.NewTicker(d)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			fn(ctx)
		}
	}
}

func (w *Worker) reap(ctx context.Context) {
	r, err := w.queue.Reap(ctx, 100)
	if err != nil {
		w.log.Warn("reap failed", "err", err)
		return
	}
	if r.Requeued+r.Failed+r.Cancelled > 0 {
		w.log.Warn("reaped expired leases", "requeued", r.Requeued, "failed", r.Failed, "cancelled", r.Cancelled)
		w.metrics.Reaped.WithLabelValues("requeued").Add(float64(r.Requeued))
		w.metrics.Reaped.WithLabelValues("failed").Add(float64(r.Failed))
		w.metrics.Reaped.WithLabelValues("cancelled").Add(float64(r.Cancelled))
		w.nudge()
	}
	if _, err := w.queue.PurgeIdempotencyKeys(ctx, 24*time.Hour); err != nil {
		w.log.Warn("idempotency purge failed", "err", err)
	}
}

func buildVersion() string {
	if bi, ok := debug.ReadBuildInfo(); ok {
		for _, s := range bi.Settings {
			if s.Key == "vcs.revision" && len(s.Value) >= 12 {
				return s.Value[:12]
			}
		}
	}
	return "dev"
}

// execute runs one claimed job to a recorded outcome.
func (w *Worker) execute(parent context.Context, job *domain.Job) {
	log := w.log.With("job_id", job.ID, "attempt", job.Attempt, "tenant_id", job.TenantID, "scenario", job.Scenario)
	token := *job.LeaseToken
	cacheKey := scenario.CacheKey(job.SpecHash, w.solverID)
	// Writes that record an outcome must survive shutdown of parent.
	finalCtx := func() (context.Context, context.CancelFunc) {
		return context.WithTimeout(context.WithoutCancel(parent), 30*time.Second)
	}

	// Worker-side cache check: catches a duplicate that was queued while
	// its twin was still running, which the submit-time check cannot.
	if hit := w.cacheLookup(parent, job, cacheKey); hit != nil {
		ctx, cancel := finalCtx()
		defer cancel()
		err := w.retry(ctx, func() error { return w.queue.CompleteFromCache(ctx, job.ID, token, hit, w.solverID, cacheKey) })
		w.finished(ctx, log, job, "cache_hit", domain.StateCompleted, err, 0)
		return
	}

	dir := filepath.Join(w.cfg.WorkDir, fmt.Sprintf("aquasph-%s-%d", job.ID, job.Attempt))
	if err := os.MkdirAll(dir, 0o750); err != nil {
		w.failInternal(parent, log, job, token, fmt.Errorf("create workdir: %w", err))
		return
	}
	defer os.RemoveAll(dir)
	scenarioPath := filepath.Join(dir, "scenario.json")
	if err := os.WriteFile(scenarioPath, job.Spec, 0o640); err != nil {
		w.failInternal(parent, log, job, token, fmt.Errorf("write scenario: %w", err))
		return
	}

	runCtx, stopRun := context.WithCancelCause(parent)
	defer stopRun(nil)
	runCtx, cancelTimeout := context.WithTimeoutCause(runCtx, time.Duration(job.TimeoutSeconds)*time.Second, errTimeout)
	defer cancelTimeout()

	var latest atomic.Pointer[domain.Progress]
	hbDone := make(chan struct{})
	hbStopped := make(chan struct{})
	go func() {
		defer close(hbStopped)
		w.heartbeat(runCtx, log, job, token, &latest, stopRun, hbDone)
	}()

	log.Info("solver starting")
	out, runErr := solver.Run(runCtx, solver.Spec{
		Binary: w.cfg.SolverPath, WorkDir: dir, ScenarioPath: scenarioPath,
		MetricsPath: filepath.Join(dir, "metrics.json"), Quality: job.Quality,
		SimTime: job.SimTime, MaxSteps: job.MaxSteps, MaxParticles: job.MaxParticles,
		Threads: w.cfg.SolverThreads, CancelGrace: w.cfg.CancelGrace,
	}, func(ev solver.Event) {
		if ev.Event != "progress" {
			return
		}
		p := &domain.Progress{T: ev.T, TEnd: ev.TEnd, Step: ev.Step, Fluid: ev.Fluid, MaxSpeed: ev.MaxSpeed, WallS: ev.WallS}
		if ev.TEnd > 0 {
			p.Fraction = min(ev.T/ev.TEnd, 1)
		}
		latest.Store(p)
		_ = w.events.Publish(parent, events.Event{Type: "progress", JobID: job.ID, Progress: p})
	})
	close(hbDone)
	<-hbStopped

	ctx, cancel := finalCtx()
	defer cancel()
	if runErr != nil {
		w.failInternal(ctx, log, job, token, fmt.Errorf("run solver: %w", runErr))
		return
	}
	w.record(ctx, log, job, token, cacheKey, dir, out, latest.Load())
}

func (w *Worker) cacheLookup(ctx context.Context, job *domain.Job, cacheKey string) *domain.Job {
	if !job.AllowCache || w.cfg.CacheScope == config.CacheOff {
		return nil
	}
	var scope *uuid.UUID
	if w.cfg.CacheScope == config.CacheTenant {
		scope = &job.TenantID
	}
	src, err := store.FindCached(ctx, w.store.Pool(), []string{cacheKey}, scope)
	if err != nil || src == nil || src.ID == job.ID {
		return nil
	}
	return src
}

// heartbeat renews the lease and relays cancellation until done closes.
// Losing the lease stops the solver at once: the job now belongs to a
// different attempt and this one's result will be rejected anyway.
func (w *Worker) heartbeat(ctx context.Context, log *slog.Logger, job *domain.Job, token uuid.UUID,
	latest *atomic.Pointer[domain.Progress], stop context.CancelCauseFunc, done <-chan struct{}) {
	t := time.NewTicker(w.cfg.HeartbeatInterval)
	defer t.Stop()
	for {
		select {
		case <-done:
			return
		case <-t.C:
		}
		var prog json.RawMessage
		if p := latest.Load(); p != nil {
			prog, _ = json.Marshal(p)
		}
		hbCtx, cancel := context.WithTimeout(context.WithoutCancel(ctx), w.cfg.HeartbeatInterval)
		res, err := w.queue.Heartbeat(hbCtx, job.ID, token, prog)
		cancel()
		switch {
		case errors.Is(err, domain.ErrLeaseLost):
			log.Warn("lease lost; stopping solver and discarding this attempt")
			stop(errLeaseLost)
			return
		case err != nil:
			// Transient: the lease is still valid until it expires, and the
			// next beat may succeed.
			log.Warn("heartbeat failed", "err", err)
		case res.CancelRequested:
			log.Info("cancellation requested")
			stop(errUserCancel)
		}
	}
}

// record turns a solver Outcome into a queue transition. Order matters:
// a lost lease trumps everything (the result is not ours to write), and a
// run that reached a result counts as that result even if a deadline
// fired in the same instant.
func (w *Worker) record(ctx context.Context, log *slog.Logger, job *domain.Job, token uuid.UUID, cacheKey, dir string,
	out *solver.Outcome, last *domain.Progress) {
	dur := out.Duration.Seconds()
	if errors.Is(out.StopCause, errLeaseLost) {
		w.metrics.Jobs.WithLabelValues("lease_lost").Inc()
		log.Warn("attempt discarded after losing its lease", "exit_code", out.ExitCode)
		return
	}

	result, _ := readResult(filepath.Join(dir, "metrics.json"))
	var progress json.RawMessage
	if last != nil {
		if out.Done != nil {
			last.T, last.Fraction, last.Step = out.Done.T, 1, out.Done.Steps
		}
		progress, _ = json.Marshal(last)
	}

	if out.ExitCode == solver.ExitStable || out.ExitCode == solver.ExitUnstable {
		if result == nil {
			w.failInternal(ctx, log, job, token,
				fmt.Errorf("solver exited %d but metrics.json is missing or not valid JSON", out.ExitCode))
			return
		}
		arts, err := w.upload(ctx, job.ID, dir)
		if err != nil {
			w.failInternal(ctx, log, job, token, fmt.Errorf("upload artifacts: %w", err))
			return
		}
		outcome := domain.OutcomeStable
		if out.ExitCode == solver.ExitUnstable {
			outcome = domain.OutcomeUnstable
		}
		err = w.retry(ctx, func() error {
			return w.queue.Complete(ctx, job.ID, token, queue.Completion{
				Outcome: outcome, Result: result, Artifacts: arts, SolverID: w.solverID, CacheKey: cacheKey, Progress: progress})
		})
		w.finished(ctx, log, job, "completed_"+outcome, domain.StateCompleted, err, dur)
		return
	}

	partial := func() *queue.Partial {
		arts, err := w.upload(ctx, job.ID, dir)
		if err != nil {
			log.Warn("could not store partial artifacts", "err", err)
			return &queue.Partial{Result: result}
		}
		return &queue.Partial{Result: result, Artifacts: arts}
	}

	switch {
	case errors.Is(out.StopCause, errShutdown):
		err := w.retry(ctx, func() error { return w.queue.Release(ctx, job.ID, token) })
		w.finished(ctx, log, job, "released", domain.StateQueued, err, dur)
	case errors.Is(out.StopCause, errUserCancel):
		p := partial()
		err := w.retry(ctx, func() error { return w.queue.Cancelled(ctx, job.ID, token, p) })
		w.finished(ctx, log, job, "cancelled", domain.StateCancelled, err, dur)
	case errors.Is(out.StopCause, errTimeout):
		w.fail(ctx, log, job, token, domain.ErrCodeTimeout,
			fmt.Sprintf("exceeded the %ds wall-clock limit; partial metrics kept", job.TimeoutSeconds), false, partial(), dur)
	case out.ExitCode == solver.ExitUsage:
		w.fail(ctx, log, job, token, domain.ErrCodeInvalidScenario, lastLines(out.StderrTail, 5), false, nil, dur)
	case out.Signal != "":
		w.fail(ctx, log, job, token, domain.ErrCodeSolverCrashed,
			fmt.Sprintf("solver killed by signal: %s. %s", out.Signal, lastLines(out.StderrTail, 5)), true, partial(), dur)
	default:
		w.fail(ctx, log, job, token, domain.ErrCodeSolverCrashed,
			fmt.Sprintf("solver exited with unexpected code %d. %s", out.ExitCode, lastLines(out.StderrTail, 5)), true, partial(), dur)
	}
}

func (w *Worker) fail(ctx context.Context, log *slog.Logger, job *domain.Job, token uuid.UUID, code, msg string,
	retryable bool, p *queue.Partial, dur float64) {
	var state domain.JobState
	err := w.retry(ctx, func() error {
		var err error
		state, err = w.queue.Fail(ctx, job.ID, token, code, msg, retryable, p)
		return err
	})
	result := "failed_" + code
	if state == domain.StateQueued {
		result = "requeued"
	}
	w.finished(ctx, log.With("error_code", code, "error", msg), job, result, state, err, dur)
}

// failInternal records a failure of the worker's own machinery (disk,
// artifact store). It is retryable: another worker, or this one later,
// may well succeed.
func (w *Worker) failInternal(ctx context.Context, log *slog.Logger, job *domain.Job, token uuid.UUID, cause error) {
	log.Error("internal failure", "err", cause)
	w.fail(ctx, log, job, token, domain.ErrCodeInternal, cause.Error(), true, nil, 0)
}

func (w *Worker) finished(ctx context.Context, log *slog.Logger, job *domain.Job, result string, state domain.JobState,
	err error, dur float64) {
	switch {
	case errors.Is(err, domain.ErrLeaseLost):
		result = "lease_lost"
		log.Warn("lease lost before the outcome could be recorded; discarded")
	case err != nil:
		// Giving up is safe: the lease will expire and the reaper will
		// requeue the job. The cost is re-running it, never losing it.
		result = "record_failed"
		log.Error("could not record outcome; the reaper will recover the job", "err", err)
	default:
		log.Info("job finished", "result", result, "state", state, "solver_s", dur)
		_ = w.events.Publish(ctx, events.Event{Type: "state", JobID: job.ID, State: state})
	}
	w.metrics.Jobs.WithLabelValues(result).Inc()
	if dur > 0 {
		w.metrics.JobDuration.WithLabelValues(result).Observe(dur)
	}
}

// retry retries fn on transient errors with capped backoff. A lost lease
// is not transient and is returned at once.
func (w *Worker) retry(ctx context.Context, fn func() error) error {
	delay := 100 * time.Millisecond
	var err error
	for attempt := 0; attempt < 6; attempt++ {
		if err = fn(); err == nil || errors.Is(err, domain.ErrLeaseLost) {
			return err
		}
		select {
		case <-ctx.Done():
			return err
		case <-time.After(delay):
		}
		delay = min(delay*2, 3*time.Second)
	}
	return err
}

// Files uploaded from the workdir, in the order they are listed.
var artifactFiles = []string{"metrics.json", "progress.jsonl", "stderr.log", "scenario.json"}

func (w *Worker) upload(ctx context.Context, jobID uuid.UUID, dir string) (json.RawMessage, error) {
	var list []domain.Artifact
	for _, name := range artifactFiles {
		f, err := os.Open(filepath.Join(dir, name))
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			return nil, err
		}
		a, err := w.artifacts.Put(ctx, jobID, name, f)
		f.Close()
		if err != nil {
			return nil, err
		}
		list = append(list, a)
	}
	return json.Marshal(list)
}

func readResult(path string) (json.RawMessage, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	if !json.Valid(b) {
		return nil, fmt.Errorf("invalid JSON")
	}
	return b, nil
}

// lastLines returns the final n lines of s, which is where a crashing
// program's explanation usually is.
func lastLines(s string, n int) string {
	lines := strings.Split(strings.TrimRight(s, "\n"), "\n")
	if len(lines) > n {
		lines = lines[len(lines)-n:]
	}
	return strings.Join(lines, "\n")
}
