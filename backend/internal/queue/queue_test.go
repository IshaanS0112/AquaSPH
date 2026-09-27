package queue_test

import (
	"context"
	"encoding/json"
	"errors"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/IshaanS0112/AquaSPH/backend/internal/ids"
	"github.com/IshaanS0112/AquaSPH/backend/internal/queue"
	"github.com/IshaanS0112/AquaSPH/backend/internal/store"
	"github.com/IshaanS0112/AquaSPH/backend/internal/testutil"
	"github.com/google/uuid"
	"github.com/jackc/pgx/v5/pgxpool"
)

var ctx = context.Background()

func newQueue(t *testing.T) (*queue.Queue, *pgxpool.Pool) {
	t.Helper()
	pool, _ := testutil.NewDB(t)
	return queue.New(pool, queue.Config{Lease: 30 * time.Second, RetryBase: 0, RetryMax: 0}), pool
}

func newTenant(t *testing.T, pool *pgxpool.Pool, maxConcurrent int) uuid.UUID {
	t.Helper()
	id := ids.New()
	_, err := pool.Exec(ctx, `INSERT INTO tenants (id, name, max_concurrent_jobs) VALUES ($1, $2, $3)`,
		id, "t"+id.String()[24:], maxConcurrent)
	if err != nil {
		t.Fatal(err)
	}
	return id
}

func enqueue(t *testing.T, pool *pgxpool.Pool, tenant uuid.UUID, priority int) uuid.UUID {
	t.Helper()
	id := ids.New()
	_, err := pool.Exec(ctx, `
		INSERT INTO jobs (id, tenant_id, priority, scenario, quality, max_particles, timeout_seconds, spec, spec_hash)
		VALUES ($1, $2, $3, 'dam_break', 'low', 1000, 60, '{}', 'h')`, id, tenant, priority)
	if err != nil {
		t.Fatal(err)
	}
	return id
}

func getJob(t *testing.T, pool *pgxpool.Pool, id uuid.UUID) *domain.Job {
	t.Helper()
	j, err := store.ScanJob(pool.QueryRow(ctx, `SELECT `+store.JobColumns+` FROM jobs WHERE id = $1`, id))
	if err != nil {
		t.Fatal(err)
	}
	return j
}

func expireLease(t *testing.T, pool *pgxpool.Pool, id uuid.UUID) {
	t.Helper()
	if _, err := pool.Exec(ctx, `UPDATE jobs SET lease_expires_at = now() - interval '1 second' WHERE id = $1`, id); err != nil {
		t.Fatal(err)
	}
}

func mustClaim(t *testing.T, q *queue.Queue, worker uuid.UUID) *domain.Job {
	t.Helper()
	j, err := q.Claim(ctx, worker)
	if err != nil {
		t.Fatal(err)
	}
	if j == nil {
		t.Fatal("expected a job, queue was empty")
	}
	return j
}

func complete(q *queue.Queue, j *domain.Job) error {
	return q.Complete(ctx, j.ID, *j.LeaseToken, queue.Completion{
		Outcome: domain.OutcomeStable, Result: json.RawMessage(`{"status":"STABLE"}`),
		SolverID: "s", CacheKey: "k",
	})
}

func TestClaimOnEmptyQueueReturnsNil(t *testing.T) {
	t.Parallel()
	q, _ := newQueue(t)
	j, err := q.Claim(ctx, ids.New())
	if err != nil || j != nil {
		t.Fatalf("got (%v, %v), want (nil, nil)", j, err)
	}
}

func TestClaimOrdersByPriorityThenFIFO(t *testing.T) {
	t.Parallel()
	q, pool := newQueue(t)
	tn := newTenant(t, pool, 100)
	low1 := enqueue(t, pool, tn, 1)
	high := enqueue(t, pool, tn, 9)
	low2 := enqueue(t, pool, tn, 1)

	w := ids.New()
	for _, want := range []uuid.UUID{high, low1, low2} {
		if got := mustClaim(t, q, w); got.ID != want {
			t.Fatalf("claimed %s, want %s", got.ID, want)
		}
	}
}

func TestClaimSetsLeaseAndChargesAttempt(t *testing.T) {
	t.Parallel()
	q, pool := newQueue(t)
	tn := newTenant(t, pool, 1)
	enqueue(t, pool, tn, 5)
	w := ids.New()
	j := mustClaim(t, q, w)
	if j.State != domain.StateRunning || j.Attempt != 1 || j.LeaseToken == nil ||
		j.WorkerID == nil || *j.WorkerID != w || j.StartedAt == nil {
		t.Fatalf("claimed job not set up for running: %+v", j)
	}
	if until := time.Until(*j.LeaseExpiresAt); until < 25*time.Second || until > 31*time.Second {
		t.Fatalf("lease expires in %s, want ~30s", until)
	}
}

func TestClaimRespectsRunAfter(t *testing.T) {
	t.Parallel()
	q, pool := newQueue(t)
	tn := newTenant(t, pool, 10)
	id := enqueue(t, pool, tn, 5)
	pool.Exec(ctx, `UPDATE jobs SET run_after = now() + interval '1 hour' WHERE id = $1`, id)
	if j, _ := q.Claim(ctx, ids.New()); j != nil {
		t.Fatal("claimed a job whose run_after is in the future")
	}
}

// The central queue property: N workers racing over M jobs complete every
// job exactly once. The audit trail, written by a trigger, is the witness.
func TestConcurrentWorkersCompleteEachJobExactlyOnce(t *testing.T) {
	t.Parallel()
	q, pool := newQueue(t)
	const jobs, workers = 200, 8
	tn := newTenant(t, pool, workers)
	for i := 0; i < jobs; i++ {
		enqueue(t, pool, tn, 5)
	}

	var completed atomic.Int64
	var wg sync.WaitGroup
	errs := make(chan error, workers)
	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			id := ids.New()
			for {
				j, err := q.Claim(ctx, id)
				if err != nil {
					errs <- err
					return
				}
				if j == nil {
					return
				}
				if err := complete(q, j); err != nil {
					errs <- err
					return
				}
				completed.Add(1)
			}
		}()
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Fatal(err)
	}
	if completed.Load() != jobs {
		t.Fatalf("completed %d, want %d", completed.Load(), jobs)
	}
	var dupes, notDone int
	pool.QueryRow(ctx, `SELECT count(*) FROM (
		SELECT job_id FROM job_events WHERE to_state = 'running' GROUP BY job_id HAVING count(*) <> 1) d`).Scan(&dupes)
	pool.QueryRow(ctx, `SELECT count(*) FROM jobs WHERE state <> 'completed'`).Scan(&notDone)
	if dupes != 0 || notDone != 0 {
		t.Fatalf("%d jobs claimed more than once, %d not completed", dupes, notDone)
	}
}

// Eight workers claim simultaneously for a tenant limited to two. Without
// the tenant row lock in claimOnce this fails intermittently (several
// claims see running=1 and all proceed); with it, exactly two win.
func TestTenantConcurrencyLimitHoldsUnderSimultaneousClaims(t *testing.T) {
	t.Parallel()
	q, pool := newQueue(t)
	const limit, workers = 2, 8
	for round := 0; round < 15; round++ {
		tn := newTenant(t, pool, limit)
		for i := 0; i < workers*2; i++ {
			enqueue(t, pool, tn, 5)
		}
		var wg sync.WaitGroup
		var won atomic.Int64
		start := make(chan struct{})
		for w := 0; w < workers; w++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				<-start
				j, err := q.Claim(ctx, ids.New())
				if err != nil {
					t.Error(err)
				}
				if j != nil {
					won.Add(1)
				}
			}()
		}
		close(start)
		wg.Wait()
		var running int
		pool.QueryRow(ctx, `SELECT count(*) FROM jobs WHERE tenant_id = $1 AND state = 'running'`, tn).Scan(&running)
		if won.Load() != limit || running != limit {
			t.Fatalf("round %d: %d claims succeeded, %d running; limit is %d", round, won.Load(), running, limit)
		}
		pool.Exec(ctx, `UPDATE jobs SET state = 'cancelled', finished_at = now(), lease_token = NULL,
			lease_expires_at = NULL WHERE tenant_id = $1`, tn)
	}
}

func TestSaturatedTenantDoesNotBlockOtherTenants(t *testing.T) {
	t.Parallel()
	q, pool := newQueue(t)
	busy := newTenant(t, pool, 1)
	other := newTenant(t, pool, 1)
	for i := 0; i < 5; i++ {
		enqueue(t, pool, busy, 9) // higher priority, and older
	}
	otherJob := enqueue(t, pool, other, 1)

	w := ids.New()
	first := mustClaim(t, q, w)
	if first.TenantID != busy {
		t.Fatal("expected the busy tenant's high-priority job first")
	}
	second := mustClaim(t, q, w)
	if second.ID != otherJob {
		t.Fatalf("claimed %s; the saturated tenant's backlog blocked the other tenant", second.ID)
	}
	if j, _ := q.Claim(ctx, w); j != nil {
		t.Fatal("both tenants are at their limit; nothing should be claimable")
	}
}

// Fencing: a worker whose lease expired and whose job was reclaimed must
// not be able to write anything. This is the zombie-worker scenario (a
// long GC pause or network partition, then the worker wakes up).
func TestExpiredLeaseHolderIsFencedOut(t *testing.T) {
	t.Parallel()
	q, pool := newQueue(t)
	tn := newTenant(t, pool, 5)
	enqueue(t, pool, tn, 5)

	zombie := mustClaim(t, q, ids.New())
	expireLease(t, pool, zombie.ID)
	if r, err := q.Reap(ctx, 10); err != nil || r.Requeued != 1 {
		t.Fatalf("reap: %+v %v", r, err)
	}
	live := mustClaim(t, q, ids.New())
	if live.ID != zombie.ID || live.Attempt != 2 || *live.LeaseToken == *zombie.LeaseToken {
		t.Fatalf("reclaim did not issue a new lease: %+v", live)
	}

	tok := *zombie.LeaseToken
	if _, err := q.Heartbeat(ctx, zombie.ID, tok, nil); !errors.Is(err, domain.ErrLeaseLost) {
		t.Errorf("zombie heartbeat: %v", err)
	}
	if err := complete(q, zombie); !errors.Is(err, domain.ErrLeaseLost) {
		t.Errorf("zombie complete: %v", err)
	}
	if _, err := q.Fail(ctx, zombie.ID, tok, "x", "y", true, nil); !errors.Is(err, domain.ErrLeaseLost) {
		t.Errorf("zombie fail: %v", err)
	}
	if err := q.Cancelled(ctx, zombie.ID, tok, nil); !errors.Is(err, domain.ErrLeaseLost) {
		t.Errorf("zombie cancel: %v", err)
	}
	if err := q.Release(ctx, zombie.ID, tok); !errors.Is(err, domain.ErrLeaseLost) {
		t.Errorf("zombie release: %v", err)
	}
	if err := complete(q, live); err != nil {
		t.Fatalf("live worker could not complete: %v", err)
	}
}

func TestReaperFailsJobAfterMaxAttempts(t *testing.T) {
	t.Parallel()
	q, pool := newQueue(t)
	tn := newTenant(t, pool, 5)
	id := enqueue(t, pool, tn, 5)
	for attempt := 1; attempt <= 3; attempt++ {
		mustClaim(t, q, ids.New())
		expireLease(t, pool, id)
		r, err := q.Reap(ctx, 10)
		if err != nil {
			t.Fatal(err)
		}
		if attempt < 3 && r.Requeued != 1 || attempt == 3 && r.Failed != 1 {
			t.Fatalf("attempt %d: %+v", attempt, r)
		}
	}
	j := getJob(t, pool, id)
	if j.State != domain.StateFailed || *j.ErrorCode != domain.ErrCodeLeaseExpired || j.FinishedAt == nil {
		t.Fatalf("job after 3 lost leases: state=%s code=%v", j.State, j.ErrorCode)
	}
}

func TestReaperIgnoresLiveLeases(t *testing.T) {
	t.Parallel()
	q, pool := newQueue(t)
	tn := newTenant(t, pool, 5)
	enqueue(t, pool, tn, 5)
	mustClaim(t, q, ids.New())
	if r, _ := q.Reap(ctx, 10); r != (queue.ReapResult{}) {
		t.Fatalf("reaped a job with a live lease: %+v", r)
	}
}

func TestRetryableFailureRequeuesThenFails(t *testing.T) {
	t.Parallel()
	q, pool := newQueue(t)
	tn := newTenant(t, pool, 5)
	id := enqueue(t, pool, tn, 5)
	for attempt := 1; attempt <= 3; attempt++ {
		j := mustClaim(t, q, ids.New())
		state, err := q.Fail(ctx, j.ID, *j.LeaseToken, domain.ErrCodeSolverCrashed, "SIGSEGV", true,
			&queue.Partial{Result: json.RawMessage(`{"partial":true}`)})
		if err != nil {
			t.Fatal(err)
		}
		want := domain.StateQueued
		if attempt == 3 {
			want = domain.StateFailed
		}
		if state != want {
			t.Fatalf("attempt %d: state %s, want %s", attempt, state, want)
		}
	}
	j := getJob(t, pool, id)
	if string(j.Result) != `{"partial": true}` {
		t.Fatalf("terminal failure should keep the partial result, got %s", j.Result)
	}
}

func TestNonRetryableFailureIsTerminal(t *testing.T) {
	t.Parallel()
	q, pool := newQueue(t)
	tn := newTenant(t, pool, 5)
	enqueue(t, pool, tn, 5)
	j := mustClaim(t, q, ids.New())
	state, err := q.Fail(ctx, j.ID, *j.LeaseToken, domain.ErrCodeInvalidScenario, "bad", false, nil)
	if err != nil || state != domain.StateFailed {
		t.Fatalf("got %s, %v", state, err)
	}
}

func TestHeartbeatRenewsLeaseStoresProgressAndReportsCancel(t *testing.T) {
	t.Parallel()
	q, pool := newQueue(t)
	tn := newTenant(t, pool, 5)
	enqueue(t, pool, tn, 5)
	j := mustClaim(t, q, ids.New())
	pool.Exec(ctx, `UPDATE jobs SET lease_expires_at = now() + interval '1 second' WHERE id = $1`, j.ID)

	hb, err := q.Heartbeat(ctx, j.ID, *j.LeaseToken, json.RawMessage(`{"t":0.25}`))
	if err != nil || hb.CancelRequested {
		t.Fatalf("heartbeat: %+v %v", hb, err)
	}
	got := getJob(t, pool, j.ID)
	if time.Until(*got.LeaseExpiresAt) < 25*time.Second || string(got.Progress) != `{"t": 0.25}` {
		t.Fatalf("lease not renewed or progress not stored: %v %s", got.LeaseExpiresAt, got.Progress)
	}

	pool.Exec(ctx, `UPDATE jobs SET cancel_requested_at = now() WHERE id = $1`, j.ID)
	hb, err = q.Heartbeat(ctx, j.ID, *j.LeaseToken, nil)
	if err != nil || !hb.CancelRequested {
		t.Fatalf("cancel request not reported: %+v %v", hb, err)
	}
	if got := getJob(t, pool, j.ID); string(got.Progress) != `{"t": 0.25}` {
		t.Fatalf("nil progress must keep the previous value, got %s", got.Progress)
	}
}

func TestReleaseReturnsJobWithoutChargingAnAttempt(t *testing.T) {
	t.Parallel()
	q, pool := newQueue(t)
	tn := newTenant(t, pool, 5)
	enqueue(t, pool, tn, 5)
	j := mustClaim(t, q, ids.New())
	if err := q.Release(ctx, j.ID, *j.LeaseToken); err != nil {
		t.Fatal(err)
	}
	again := mustClaim(t, q, ids.New())
	if again.Attempt != 1 {
		t.Fatalf("attempt %d after release, want 1", again.Attempt)
	}
}

func TestReleaseOfCancelRequestedJobCancelsIt(t *testing.T) {
	t.Parallel()
	q, pool := newQueue(t)
	tn := newTenant(t, pool, 5)
	id := enqueue(t, pool, tn, 5)
	j := mustClaim(t, q, ids.New())
	pool.Exec(ctx, `UPDATE jobs SET cancel_requested_at = now() WHERE id = $1`, id)
	if err := q.Release(ctx, j.ID, *j.LeaseToken); err != nil {
		t.Fatal(err)
	}
	if s := getJob(t, pool, id).State; s != domain.StateCancelled {
		t.Fatalf("state %s, want cancelled", s)
	}
}

func TestReapCancelsRatherThanRetriesACancelRequestedJob(t *testing.T) {
	t.Parallel()
	q, pool := newQueue(t)
	tn := newTenant(t, pool, 5)
	id := enqueue(t, pool, tn, 5)
	mustClaim(t, q, ids.New())
	pool.Exec(ctx, `UPDATE jobs SET cancel_requested_at = now() WHERE id = $1`, id)
	expireLease(t, pool, id)
	if r, _ := q.Reap(ctx, 10); r.Cancelled != 1 {
		t.Fatalf("%+v", r)
	}
}

func TestCompleteFromCacheSharesTheSourceArtifacts(t *testing.T) {
	t.Parallel()
	q, pool := newQueue(t)
	tn := newTenant(t, pool, 5)
	enqueue(t, pool, tn, 5)
	src := mustClaim(t, q, ids.New())
	if err := q.Complete(ctx, src.ID, *src.LeaseToken, queue.Completion{
		Outcome: domain.OutcomeStable, Result: json.RawMessage(`{"a":1}`),
		Artifacts: json.RawMessage(`[{"name":"metrics.json"}]`), SolverID: "s", CacheKey: "k"}); err != nil {
		t.Fatal(err)
	}
	source := getJob(t, pool, src.ID)

	// A hit on a hit must still point at the job that owns the files.
	for i := 0; i < 2; i++ {
		enqueue(t, pool, tn, 5)
		dup := mustClaim(t, q, ids.New())
		if err := q.CompleteFromCache(ctx, dup.ID, *dup.LeaseToken, source, "s", "k"); err != nil {
			t.Fatal(err)
		}
		got := getJob(t, pool, dup.ID)
		if !got.CacheHit || *got.ArtifactJobID != src.ID || string(got.Result) != `{"a": 1}` {
			t.Fatalf("hit %d: %+v", i, got)
		}
		source = got
	}
}

func TestAuditTrailIsWrittenByTheDatabase(t *testing.T) {
	t.Parallel()
	q, pool := newQueue(t)
	tn := newTenant(t, pool, 5)
	id := enqueue(t, pool, tn, 5)
	j := mustClaim(t, q, ids.New())
	q.Fail(ctx, j.ID, *j.LeaseToken, domain.ErrCodeSolverCrashed, "boom", true, nil)
	j = mustClaim(t, q, ids.New())
	complete(q, j)

	rows, _ := pool.Query(ctx, `SELECT coalesce(from_state::text, '-'), to_state::text, attempt, coalesce(detail, '')
		FROM job_events WHERE job_id = $1 ORDER BY id`, id)
	var got []string
	for rows.Next() {
		var from, to, detail string
		var attempt int
		rows.Scan(&from, &to, &attempt, &detail)
		got = append(got, from+">"+to+":"+string(rune('0'+attempt))+":"+detail)
	}
	want := []string{"->queued:0:", "queued>running:1:", "running>queued:1:solver_crashed",
		"queued>running:2:", "running>completed:2:"}
	if len(got) != len(want) {
		t.Fatalf("events %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("event %d: %q, want %q (all: %v)", i, got[i], want[i], got)
		}
	}
}

// The state invariants live in CHECK constraints, so even a buggy future
// code path, or a human with psql, cannot write an impossible job.
func TestDatabaseRejectsImpossibleJobStates(t *testing.T) {
	t.Parallel()
	_, pool := newQueue(t)
	tn := newTenant(t, pool, 5)
	cases := map[string]string{
		"running without a lease":    `UPDATE jobs SET state = 'running' WHERE id = $1`,
		"completed without outcome":  `UPDATE jobs SET state = 'completed', finished_at = now() WHERE id = $1`,
		"failed without error code":  `UPDATE jobs SET state = 'failed', finished_at = now() WHERE id = $1`,
		"terminal without finish":    `UPDATE jobs SET state = 'cancelled' WHERE id = $1`,
		"queued holding a lease":     `UPDATE jobs SET lease_token = gen_random_uuid() WHERE id = $1`,
		"cache hit without artifact": `UPDATE jobs SET state = 'completed', outcome = 'stable', finished_at = now(), cache_hit = true WHERE id = $1`,
	}
	for name, sql := range cases {
		id := enqueue(t, pool, tn, 5)
		if _, err := pool.Exec(ctx, sql, id); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
}

func TestListenWakesOnNotify(t *testing.T) {
	t.Parallel()
	pool, url := testutil.NewDB(t)
	wake := make(chan struct{}, 1)
	lctx, cancel := context.WithCancel(ctx)
	defer cancel()
	go queue.Listen(lctx, url, wake, testutil.Logger(t))

	select { // the connect-time wake
	case <-wake:
	case <-time.After(5 * time.Second):
		t.Fatal("no wake after connecting")
	}
	if _, err := pool.Exec(ctx, `SELECT pg_notify($1, '')`, queue.NotifyChannel); err != nil {
		t.Fatal(err)
	}
	select {
	case <-wake:
	case <-time.After(5 * time.Second):
		t.Fatal("NOTIFY did not wake the listener")
	}
}
