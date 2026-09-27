// Package e2e runs the real system: aquasph-api and several
// aquasph-worker processes (built from this module), the real C++ solver,
// Postgres and Redis. It is the automated form of the checks in
// docs/platform/verification.md, including failure injection: workers are
// SIGKILLed and SIGTERMed mid-job and the platform must recover.
//
// Needs the solver built at ../../build/aquasph (or AQUASPH_E2E_SOLVER).
// Skipped with -short or when the solver is missing, unless
// AQUASPH_REQUIRE_SOLVER=1, as in CI.
package e2e

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/IshaanS0112/AquaSPH/backend/internal/client"
	"github.com/IshaanS0112/AquaSPH/backend/internal/service"
	"github.com/IshaanS0112/AquaSPH/backend/internal/store"
	"github.com/IshaanS0112/AquaSPH/backend/internal/testutil"
	"github.com/jackc/pgx/v5/pgxpool"
)

type stack struct {
	t       *testing.T
	bin     string
	env     []string
	pool    *pgxpool.Pool
	c       *client.Client
	logDir  string
	mu      sync.Mutex
	workers map[int]*exec.Cmd // pid -> process
	nextID  int
}

func freeAddr(t *testing.T) string {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	return ln.Addr().String()
}

func solverPath(t *testing.T) string {
	p := os.Getenv("AQUASPH_E2E_SOLVER")
	if p == "" {
		p, _ = filepath.Abs("../../build/aquasph")
	}
	if _, err := os.Stat(p); err != nil {
		if os.Getenv("AQUASPH_REQUIRE_SOLVER") == "1" {
			t.Fatalf("solver not found at %s", p)
		}
		t.Skipf("solver not built at %s", p)
	}
	return p
}

func newStack(t *testing.T, workers int) *stack {
	if testing.Short() {
		t.Skip("e2e: skipped with -short")
	}
	solver := solverPath(t)
	bin := t.TempDir()
	build := exec.Command("go", "build", "-o", bin+"/", "../cmd/aquasph-api", "../cmd/aquasph-worker")
	if out, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}
	pool, dbURL := testutil.NewDB(t)
	scen, _ := filepath.Abs("../../configs/scenarios")
	public := freeAddr(t)
	s := &stack{t: t, bin: bin, pool: pool, logDir: t.TempDir(), workers: map[int]*exec.Cmd{}}
	s.env = append(os.Environ(),
		"AQUASPH_DATABASE_URL="+dbURL,
		"AQUASPH_REDIS_URL="+testutil.RedisURL(t),
		"AQUASPH_SCENARIO_DIR="+scen,
		"AQUASPH_ARTIFACT_DIR="+t.TempDir(),
		"AQUASPH_SOLVER_PATH="+solver,
		"AQUASPH_SOLVER_THREADS=2",
		"AQUASPH_LEASE_DURATION=3s", "AQUASPH_HEARTBEAT_INTERVAL=500ms", "AQUASPH_REAP_INTERVAL=1s",
		"AQUASPH_POLL_INTERVAL=200ms", "AQUASPH_RETRY_BASE=200ms", "AQUASPH_DRAIN_TIMEOUT=1s",
		"AQUASPH_LOG_FORMAT=text",
	)
	api := s.start("api", "aquasph-api", "AQUASPH_HTTP_ADDR="+public, "AQUASPH_INTERNAL_ADDR="+freeAddr(t))
	t.Cleanup(func() { api.Process.Signal(syscall.SIGTERM); api.Wait() })
	base := "http://" + public
	deadline := time.Now().Add(20 * time.Second)
	for {
		if r, err := http.Get(base + "/readyz"); err == nil && r.StatusCode == 200 {
			r.Body.Close()
			break
		}
		if time.Now().After(deadline) {
			s.dumpLogs()
			t.Fatal("API never became ready")
		}
		time.Sleep(100 * time.Millisecond)
	}
	for range workers {
		s.addWorker()
	}
	st := store.New(pool)
	rate := 100000
	tn, err := st.CreateTenant(context.Background(), "e2e", store.TenantLimits{RatePerMinute: &rate, RateBurst: &rate})
	if err != nil {
		t.Fatal(err)
	}
	key, _, err := st.CreateAPIKey(context.Background(), tn.ID, "e2e")
	if err != nil {
		t.Fatal(err)
	}
	s.c = client.New(base, key)
	s.waitForWorkers(workers)
	return s
}

func (s *stack) start(name, binary string, env ...string) *exec.Cmd {
	s.t.Helper()
	logf, err := os.Create(filepath.Join(s.logDir, name+".log"))
	if err != nil {
		s.t.Fatal(err)
	}
	cmd := exec.Command(filepath.Join(s.bin, binary))
	cmd.Env = append(append([]string{}, s.env...), env...)
	cmd.Stdout, cmd.Stderr = logf, logf
	if err := cmd.Start(); err != nil {
		s.t.Fatal(err)
	}
	return cmd
}

func (s *stack) addWorker() {
	s.mu.Lock()
	s.nextID++
	name := fmt.Sprintf("worker%d", s.nextID)
	s.mu.Unlock()
	cmd := s.start(name, "aquasph-worker", "AQUASPH_INTERNAL_ADDR="+freeAddr(s.t))
	s.mu.Lock()
	s.workers[cmd.Process.Pid] = cmd
	s.mu.Unlock()
	s.t.Cleanup(func() {
		if cmd.ProcessState == nil {
			cmd.Process.Signal(syscall.SIGTERM)
			cmd.Wait()
		}
	})
}

func (s *stack) waitForWorkers(n int) {
	s.t.Helper()
	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		var live int
		s.pool.QueryRow(context.Background(), `SELECT count(*) FROM workers WHERE stopped_at IS NULL`).Scan(&live)
		if live >= n {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	s.dumpLogs()
	s.t.Fatalf("fewer than %d workers registered", n)
}

func (s *stack) dumpLogs() {
	files, _ := filepath.Glob(filepath.Join(s.logDir, "*.log"))
	for _, f := range files {
		b, _ := os.ReadFile(f)
		if len(b) > 4000 {
			b = b[len(b)-4000:]
		}
		s.t.Logf("---- %s ----\n%s", filepath.Base(f), b)
	}
}

// workerRunning returns the PID of the worker process holding job id.
func (s *stack) workerRunning(id string) int {
	var pid int
	err := s.pool.QueryRow(context.Background(), `
		SELECT w.pid FROM jobs j JOIN workers w ON w.id = j.worker_id
		WHERE j.id = $1 AND j.state = 'running'`, id).Scan(&pid)
	if err != nil {
		s.t.Fatalf("no worker holds %s: %v", id, err)
	}
	return pid
}

func (s *stack) waitRunning(id string) *client.Job {
	s.t.Helper()
	deadline := time.Now().Add(60 * time.Second)
	for time.Now().Before(deadline) {
		j, err := s.c.GetJob(context.Background(), id)
		if err != nil {
			s.t.Fatal(err)
		}
		if j.State == "running" && j.Progress != nil && j.Progress.Step > 0 {
			return j
		}
		if j.Terminal() {
			s.t.Fatalf("job %s finished (%s) before it could be observed running", id, j.State)
		}
		time.Sleep(50 * time.Millisecond)
	}
	s.t.Fatalf("job %s never reported progress", id)
	return nil
}

func (s *stack) wait(id string) *client.Job {
	s.t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 4*time.Minute)
	defer cancel()
	j, err := s.c.WaitFor(ctx, id, 200*time.Millisecond)
	if err != nil {
		s.dumpLogs()
		s.t.Fatalf("waiting for %s: %v", id, err)
	}
	return j
}

func (s *stack) history(id string) string {
	h, err := s.c.History(context.Background(), id)
	if err != nil {
		s.t.Fatal(err)
	}
	var v struct {
		Events []struct {
			From   *string `json:"from_state"`
			To     string  `json:"to_state"`
			Detail *string `json:"detail"`
		} `json:"events"`
	}
	json.Unmarshal(h, &v)
	var parts []string
	for _, e := range v.Events {
		p := e.To
		if e.Detail != nil {
			p += "(" + *e.Detail + ")"
		}
		parts = append(parts, p)
	}
	return strings.Join(parts, ">")
}

func damBreak(simTime float64) service.JobRequest {
	return service.JobRequest{Scenario: "dam_break", Quality: "low", SimTime: &simTime}
}

func procState(pid int) string {
	b, err := os.ReadFile("/proc/" + strconv.Itoa(pid) + "/stat")
	if err != nil {
		return "gone"
	}
	f := strings.Fields(string(b[strings.LastIndexByte(string(b), ')')+1:]))
	return f[0]
}

func TestPlatformEndToEnd(t *testing.T) {
	s := newStack(t, 2)
	ctx := context.Background()

	t.Run("a job runs on the real solver and its artifacts match its result", func(t *testing.T) {
		j, err := s.c.SubmitJob(ctx, damBreak(0.05), "")
		if err != nil {
			t.Fatal(err)
		}
		j = s.wait(j.ID)
		if j.State != "completed" || *j.Outcome != "stable" || j.Attempt != 1 {
			t.Fatalf("%+v", j)
		}
		res, _ := s.c.Result(ctx, j.ID)
		file, err := s.c.Download(ctx, j.ID, "metrics.json")
		if err != nil {
			t.Fatal(err)
		}
		var a, b map[string]any
		json.Unmarshal(res.Metrics, &a)
		json.Unmarshal(file, &b)
		if a["status"] != "STABLE" || !reflect.DeepEqual(a, b) {
			t.Fatal("stored result and metrics.json artifact disagree")
		}
	})

	t.Run("an identical submission is served from cache with identical metrics", func(t *testing.T) {
		first, _ := s.c.SubmitJob(ctx, damBreak(0.04), "")
		first = s.wait(first.ID)
		start := time.Now()
		hit, err := s.c.SubmitJob(ctx, damBreak(0.04), "")
		if err != nil {
			t.Fatal(err)
		}
		if !hit.CacheHit || hit.State != "completed" || *hit.SourceJobID != first.ID {
			t.Fatalf("not a cache hit: %+v", hit)
		}
		ra, _ := s.c.Result(ctx, first.ID)
		rb, _ := s.c.Result(ctx, hit.ID)
		if string(ra.Metrics) != string(rb.Metrics) {
			t.Fatal("cache hit returned different metrics")
		}
		t.Logf("cache hit served in %s", time.Since(start))
	})

	t.Run("a sweep fans out and reports a results table", func(t *testing.T) {
		st := 0.03
		sw, err := s.c.SubmitSweep(ctx, service.SweepRequest{
			JobRequest: service.JobRequest{Scenario: "dam_break", Quality: "low", SimTime: &st},
			Grid:       map[string][]any{"/materials/0/viscosity": {1.0, 3.0, 6.0}},
		}, "")
		if err != nil {
			t.Fatal(err)
		}
		deadline := time.Now().Add(3 * time.Minute)
		for {
			g, _ := s.c.GetSweep(ctx, sw.ID)
			if g.State == "completed" {
				if g.Counts["completed"] != 3 {
					t.Fatalf("%+v", g)
				}
				break
			}
			if time.Now().After(deadline) {
				t.Fatalf("sweep stuck: %+v", g)
			}
			time.Sleep(200 * time.Millisecond)
		}
		body, _ := s.c.SweepResults(ctx, sw.ID, []string{"/status", "/time/steps"}, "json")
		var r struct {
			Rows []struct {
				Params  map[string]any `json:"params"`
				Metrics map[string]any `json:"metrics"`
			} `json:"rows"`
		}
		json.Unmarshal(body, &r)
		if len(r.Rows) != 3 || r.Rows[2].Params["/materials/0/viscosity"] != 6.0 || r.Rows[2].Metrics["/status"] != "STABLE" {
			t.Fatalf("%s", body)
		}
	})

	t.Run("an invalid inline spec fails once with the solver's diagnostic", func(t *testing.T) {
		j, err := s.c.SubmitJob(ctx, service.JobRequest{Spec: map[string]any{"name": "broken"}, Quality: "low"}, "")
		if err != nil {
			t.Fatal(err)
		}
		j = s.wait(j.ID)
		if j.State != "failed" || j.Error == nil || j.Error.Code != "invalid_scenario" || j.Attempt != 1 || j.Error.Message == "" {
			t.Fatalf("%+v %+v", j, j.Error)
		}
		t.Logf("solver said: %s", j.Error.Message)
	})

	t.Run("cancelling a running job stops the solver and keeps partial metrics", func(t *testing.T) {
		j, _ := s.c.SubmitJob(ctx, damBreak(30), "")
		s.waitRunning(j.ID)
		if _, err := s.c.CancelJob(ctx, j.ID); err != nil {
			t.Fatal(err)
		}
		j = s.wait(j.ID)
		res, err := s.c.Result(ctx, j.ID)
		if j.State != "cancelled" || err != nil || !res.Partial || !strings.Contains(string(res.Metrics), `"CANCELLED"`) {
			t.Fatalf("%s %+v %v", j.State, res, err)
		}
	})

	t.Run("a SIGKILLed worker's job is recovered by another worker", func(t *testing.T) {
		j, _ := s.c.SubmitJob(ctx, service.JobRequest{Scenario: "dam_break", Quality: "low",
			SimTime: ptr(0.35), Cache: ptr(false)}, "")
		s.waitRunning(j.ID)
		victim := s.workerRunning(j.ID)
		solverPID := childPID(t, victim)
		s.addWorker() // keep the pool at two
		s.mu.Lock()
		cmd := s.workers[victim]
		s.mu.Unlock()
		cmd.Process.Kill()
		cmd.Wait()

		// Pdeathsig: the solver must die with its worker. A zombie (Z)
		// waiting to be reaped by PID 1 is dead; R or S is an orphan.
		time.Sleep(100 * time.Millisecond)
		if st := procState(solverPID); st != "gone" && st != "Z" {
			t.Fatalf("orphaned solver %d still alive in state %s", solverPID, st)
		}
		j = s.wait(j.ID)
		if j.State != "completed" || j.Attempt != 2 {
			t.Fatalf("%s attempt %d", j.State, j.Attempt)
		}
		if h := s.history(j.ID); !strings.Contains(h, "queued(lease_expired)") {
			t.Fatalf("history does not show the lost lease: %s", h)
		}
	})

	t.Run("a draining worker hands its job back without charging an attempt", func(t *testing.T) {
		j, _ := s.c.SubmitJob(ctx, service.JobRequest{Scenario: "dam_break", Quality: "low",
			SimTime: ptr(0.3), Cache: ptr(false)}, "")
		s.waitRunning(j.ID)
		victim := s.workerRunning(j.ID)
		s.addWorker()
		s.mu.Lock()
		cmd := s.workers[victim]
		s.mu.Unlock()
		cmd.Process.Signal(syscall.SIGTERM)
		if err := cmd.Wait(); err != nil {
			t.Fatalf("worker did not exit cleanly on SIGTERM: %v", err)
		}
		j = s.wait(j.ID)
		if j.State != "completed" || j.Attempt != 1 {
			t.Fatalf("%s attempt %d (a release must not charge an attempt)", j.State, j.Attempt)
		}
		if h := s.history(j.ID); !strings.Contains(h, "running>queued>running>completed") {
			t.Fatalf("history: %s", h)
		}
	})
}

func childPID(t *testing.T, parent int) int {
	t.Helper()
	b, err := os.ReadFile(fmt.Sprintf("/proc/%d/task/%d/children", parent, parent))
	if err == nil {
		if f := strings.Fields(string(b)); len(f) > 0 {
			pid, _ := strconv.Atoi(f[0])
			return pid
		}
	}
	// Children of other threads: scan all tasks.
	tasks, _ := filepath.Glob(fmt.Sprintf("/proc/%d/task/*/children", parent))
	for _, tf := range tasks {
		b, _ := os.ReadFile(tf)
		if f := strings.Fields(string(b)); len(f) > 0 {
			pid, _ := strconv.Atoi(f[0])
			return pid
		}
	}
	t.Fatalf("worker %d has no solver child", parent)
	return 0
}

func ptr[T any](v T) *T { return &v }
