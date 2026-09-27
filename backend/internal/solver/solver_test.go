package solver

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

func fake(t *testing.T, mode string) Spec {
	t.Helper()
	t.Setenv("FAKE_MODE", mode)
	bin, _ := filepath.Abs("testdata/fake_solver.sh")
	dir := t.TempDir()
	return Spec{Binary: bin, WorkDir: dir, ScenarioPath: filepath.Join(dir, "scenario.json"),
		MetricsPath: filepath.Join(dir, "metrics.json"), Quality: "low", CancelGrace: 2 * time.Second}
}

func collect() (func(Event), func() []Event) {
	var mu sync.Mutex
	var evs []Event
	return func(e Event) { mu.Lock(); evs = append(evs, e); mu.Unlock() },
		func() []Event { mu.Lock(); defer mu.Unlock(); return append([]Event(nil), evs...) }
}

func TestCompletedRunReportsEventsAndExitCode(t *testing.T) {
	s := fake(t, "ok")
	on, got := collect()
	out, err := Run(context.Background(), s, on)
	if err != nil {
		t.Fatal(err)
	}
	if out.ExitCode != ExitStable || out.Done == nil || out.Done.Status != "STABLE" || out.StopCause != nil {
		t.Fatalf("%+v", out)
	}
	if evs := got(); len(evs) != 4 || evs[1].Event != "progress" || evs[2].T != 0.5 {
		t.Fatalf("events: %+v", evs)
	}
	if b, _ := os.ReadFile(out.ProgressPath); strings.Count(string(b), "\n") != 4 {
		t.Fatalf("progress.jsonl: %q", b)
	}
}

func TestExitCodesAreReportedNotInterpreted(t *testing.T) {
	for mode, want := range map[string]int{"unstable": ExitUnstable, "invalid": ExitUsage} {
		out, err := Run(context.Background(), fake(t, mode), nil)
		if err != nil || out.ExitCode != want {
			t.Errorf("%s: exit %d, err %v; want %d", mode, out.ExitCode, err, want)
		}
	}
	out, _ := Run(context.Background(), fake(t, "invalid"), nil)
	if !strings.Contains(out.StderrTail, "domain.max must exceed") {
		t.Errorf("stderr not captured: %q", out.StderrTail)
	}
}

func TestCrashIsReportedAsASignal(t *testing.T) {
	out, err := Run(context.Background(), fake(t, "crash"), nil)
	if err != nil {
		t.Fatal(err)
	}
	if out.ExitCode != -1 || out.Signal != "segmentation fault" || out.StopCause != nil || out.Done != nil {
		t.Fatalf("%+v", out)
	}
}

func TestNonJSONLinesAreCountedAndSkipped(t *testing.T) {
	out, err := Run(context.Background(), fake(t, "garbage"), nil)
	if err != nil || out.BadLines != 2 || out.Done == nil {
		t.Fatalf("%+v %v", out, err)
	}
}

// Cancellation is cooperative first: SIGTERM, and the solver writes its
// partial result and exits 3 well inside the grace period.
func TestCancelSendsSIGTERMAndKeepsThePartialResult(t *testing.T) {
	s := fake(t, "slow")
	ctx, cancel := context.WithCancelCause(context.Background())
	stop := errors.New("user asked")
	on := func(e Event) {
		if e.Event == "progress" && e.Step >= 3 {
			cancel(stop)
		}
	}
	out, err := Run(ctx, s, on)
	if err != nil {
		t.Fatal(err)
	}
	if out.ExitCode != ExitCancelled || !errors.Is(out.StopCause, stop) || out.Killed ||
		out.Done == nil || out.Done.Status != "CANCELLED" {
		t.Fatalf("%+v", out)
	}
	if b, _ := os.ReadFile(s.MetricsPath); !strings.Contains(string(b), "CANCELLED") {
		t.Fatalf("partial metrics missing: %q", b)
	}
}

// ...and forceful when that fails: a solver that ignores SIGTERM is
// SIGKILLed once the grace period expires.
func TestSolverIgnoringSIGTERMIsKilledAfterTheGracePeriod(t *testing.T) {
	s := fake(t, "stubborn")
	s.CancelGrace = 300 * time.Millisecond
	ctx, cancel := context.WithTimeout(context.Background(), 200*time.Millisecond)
	defer cancel()
	start := time.Now()
	out, err := Run(ctx, s, nil)
	if err != nil {
		t.Fatal(err)
	}
	if !out.Killed || out.Signal != "killed" || !errors.Is(out.StopCause, context.DeadlineExceeded) {
		t.Fatalf("%+v", out)
	}
	if el := time.Since(start); el > 3*time.Second {
		t.Fatalf("took %s to kill a stubborn solver", el)
	}
}

func TestArgsMapRunParametersToFlags(t *testing.T) {
	st, steps := 0.25, 1000
	s := Spec{ScenarioPath: "/w/scenario.json", MetricsPath: "/w/m.json", Quality: "medium",
		SimTime: &st, MaxSteps: &steps, MaxParticles: 5000, Threads: 4}
	got := strings.Join(s.Args(), " ")
	want := "--scenario /w/scenario.json --quality medium --metrics /w/m.json --progress-json " +
		"--time 0.25 --steps 1000 --max-particles 5000 --threads 4"
	if got != want {
		t.Fatalf("got  %s\nwant %s", got, want)
	}
}

func TestIdentityIsTheBinaryHash(t *testing.T) {
	dir := t.TempDir()
	a, b := filepath.Join(dir, "a"), filepath.Join(dir, "b")
	os.WriteFile(a, []byte("build 1"), 0o755)
	os.WriteFile(b, []byte("build 2"), 0o755)
	ia, _ := Identity(a)
	ib, _ := Identity(b)
	if ia == ib || len(ia) != 64 {
		t.Fatalf("%s %s", ia, ib)
	}
}

// The real binary, when it has been built: guards the protocol against
// drift between src/main.cpp and this package.
func TestRealSolverSpeaksTheProtocol(t *testing.T) {
	bin, _ := filepath.Abs("../../../build/aquasph")
	if _, err := os.Stat(bin); err != nil {
		if os.Getenv("AQUASPH_REQUIRE_SOLVER") == "1" {
			t.Fatalf("solver not built at %s", bin)
		}
		t.Skip("solver not built; run cmake --build build first")
	}
	scen, _ := filepath.Abs("../../../configs/scenarios/dam_break.json")
	dir := t.TempDir()
	simTime := 0.02
	out, err := Run(context.Background(), Spec{Binary: bin, WorkDir: dir, ScenarioPath: scen,
		MetricsPath: filepath.Join(dir, "metrics.json"), Quality: "low", SimTime: &simTime,
		Threads: 2, CancelGrace: 5 * time.Second}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if out.ExitCode != ExitStable || out.Start == nil || out.Done == nil || out.Done.Status != "STABLE" ||
		out.BadLines != 0 || out.Start.Fluid == 0 {
		t.Fatalf("%+v\nstderr: %s", out, out.StderrTail)
	}
}
