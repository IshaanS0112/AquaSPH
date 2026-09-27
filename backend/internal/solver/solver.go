// Package solver supervises one run of the aquasph binary: it builds the
// command line, parses the --progress-json stream, enforces cancellation
// (SIGTERM, a grace period, then SIGKILL), and reports how the process
// ended. The protocol is documented in docs/platform/TRD.md section 6.
//
// It deliberately knows nothing about jobs, leases or databases: it runs
// a process and says what happened. Deciding what that means for a job
// is the worker's job.
package solver

import (
	"bufio"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"syscall"
	"time"
)

// Exit codes of the solver CLI (src/main.cpp).
const (
	ExitStable    = 0
	ExitUnstable  = 1
	ExitUsage     = 2
	ExitCancelled = 3
)

// Event is one line of the --progress-json stream. Fields not relevant to
// an event type are zero.
type Event struct {
	Event string `json:"event"` // start | progress | done

	Scenario string  `json:"scenario,omitempty"`
	Tier     int     `json:"tier,omitempty"`
	Quality  string  `json:"quality,omitempty"`
	Boundary int64   `json:"boundary,omitempty"`
	Threads  int     `json:"threads,omitempty"`
	TEnd     float64 `json:"t_end,omitempty"`

	T          float64  `json:"t,omitempty"`
	Step       int64    `json:"step,omitempty"`
	Dt         *float64 `json:"dt,omitempty"`
	Fluid      int64    `json:"fluid,omitempty"`
	MaxSpeed   *float64 `json:"max_speed,omitempty"`
	DensityMax *float64 `json:"density_max,omitempty"`
	WallS      float64  `json:"wall_s,omitempty"`

	Status            string `json:"status,omitempty"`
	Steps             int64  `json:"steps,omitempty"`
	UnstableParticles int64  `json:"unstable_particles,omitempty"`
	ExitCode          *int   `json:"exit_code,omitempty"`
}

// Spec is one invocation.
type Spec struct {
	Binary       string
	WorkDir      string // cwd; progress.jsonl and stderr.log are written here
	ScenarioPath string
	MetricsPath  string
	Quality      string
	SimTime      *float64
	MaxSteps     *int
	MaxParticles int
	Threads      int // 0 lets OpenMP decide
	// CancelGrace is how long a SIGTERMed solver gets to write its
	// partial metrics before it is SIGKILLed.
	CancelGrace time.Duration
}

func (s Spec) Args() []string {
	args := []string{
		"--scenario", s.ScenarioPath,
		"--quality", s.Quality,
		"--metrics", s.MetricsPath,
		"--progress-json",
	}
	if s.SimTime != nil {
		args = append(args, "--time", strconv.FormatFloat(*s.SimTime, 'g', -1, 64))
	}
	if s.MaxSteps != nil {
		args = append(args, "--steps", strconv.Itoa(*s.MaxSteps))
	}
	if s.MaxParticles > 0 {
		args = append(args, "--max-particles", strconv.Itoa(s.MaxParticles))
	}
	if s.Threads > 0 {
		args = append(args, "--threads", strconv.Itoa(s.Threads))
	}
	return args
}

// Outcome is how a run ended.
type Outcome struct {
	ExitCode int    // -1 when the process was killed by a signal
	Signal   string // set when killed by a signal, e.g. "segmentation fault"
	Done     *Event // the final "done" event, if the solver got that far
	Start    *Event
	// StopCause is non-nil when the run was stopped by its context (user
	// cancel, timeout, shutdown, lost lease): context.Cause of ctx.
	StopCause    error
	Killed       bool // needed SIGKILL after the grace period
	StderrTail   string
	BadLines     int // stdout lines that were not valid JSON events
	Duration     time.Duration
	ProgressPath string
	StderrPath   string
}

// Run starts the solver and blocks until it exits. onEvent is called
// from the stdout-reading goroutine for every parsed event.
//
// Pipes are owned here rather than taken from cmd.StdoutPipe: os/exec's
// Wait closes that pipe as soon as the process exits, so calling Wait
// before the reader reaches EOF can drop the final lines -- and the final
// line is the "done" event. That happened in roughly one run in six
// before this was rewritten. Stderr goes straight to a file for the same
// reason (no copying goroutine for Wait to block on).
func Run(ctx context.Context, s Spec, onEvent func(Event)) (*Outcome, error) {
	out := &Outcome{
		ProgressPath: filepath.Join(s.WorkDir, "progress.jsonl"),
		StderrPath:   filepath.Join(s.WorkDir, "stderr.log"),
	}
	progressFile, err := os.Create(out.ProgressPath)
	if err != nil {
		return nil, err
	}
	defer progressFile.Close()
	stderrFile, err := os.Create(out.StderrPath)
	if err != nil {
		return nil, err
	}
	defer stderrFile.Close()
	pr, pw, err := os.Pipe()
	if err != nil {
		return nil, err
	}
	defer pr.Close()

	cmd := exec.Command(s.Binary, s.Args()...)
	cmd.Dir = s.WorkDir
	cmd.Env = os.Environ()
	cmd.Stdout = pw
	cmd.Stderr = stderrFile
	cmd.SysProcAttr = sysProcAttr()

	start := time.Now()
	if err := cmd.Start(); err != nil {
		pw.Close()
		return nil, fmt.Errorf("start solver: %w", err)
	}
	pw.Close() // the child holds the only write end now; EOF means it exited

	readDone := make(chan struct{})
	go func() {
		defer close(readDone)
		sc := bufio.NewScanner(pr)
		sc.Buffer(make([]byte, 64<<10), 1<<20)
		for sc.Scan() {
			line := sc.Bytes()
			progressFile.Write(line)
			progressFile.Write([]byte{'\n'})
			var ev Event
			if err := json.Unmarshal(line, &ev); err != nil || ev.Event == "" {
				out.BadLines++
				continue
			}
			switch ev.Event {
			case "start":
				e := ev
				out.Start = &e
			case "done":
				e := ev
				out.Done = &e
			}
			if onEvent != nil {
				onEvent(ev)
			}
		}
	}()

	waitCh := make(chan error, 1)
	go func() { waitCh <- cmd.Wait() }()

	var waitErr error
	select {
	case waitErr = <-waitCh:
	case <-ctx.Done():
		out.StopCause = context.Cause(ctx)
		// Ask politely: the solver finishes its step, writes partial
		// metrics and exits 3. The whole process group is signalled, so
		// nothing the solver might have spawned outlives it.
		signalGroup(cmd, syscall.SIGTERM)
		grace := time.NewTimer(s.CancelGrace)
		select {
		case waitErr = <-waitCh:
			grace.Stop()
		case <-grace.C:
			signalGroup(cmd, syscall.SIGKILL)
			out.Killed = true
			waitErr = <-waitCh
		}
	}
	// The process has exited; its group was signalled if we stopped it.
	// Bound the wait for EOF anyway, in case some descendant kept the
	// pipe open, so a misbehaving child can never wedge a worker slot.
	select {
	case <-readDone:
	case <-time.After(2 * time.Second):
		pr.Close()
		<-readDone
	}
	out.Duration = time.Since(start)
	out.StderrTail = readTail(out.StderrPath, 8<<10)

	var exitErr *exec.ExitError
	switch {
	case waitErr == nil:
		out.ExitCode = 0
	case errors.As(waitErr, &exitErr):
		ws, _ := exitErr.Sys().(syscall.WaitStatus)
		if ws.Signaled() {
			out.ExitCode = -1
			out.Signal = ws.Signal().String()
		} else {
			out.ExitCode = ws.ExitStatus()
		}
	default:
		return out, fmt.Errorf("wait for solver: %w", waitErr)
	}
	return out, nil
}

func signalGroup(cmd *exec.Cmd, sig syscall.Signal) {
	// Negative PID: the process group led by the solver (Setpgid).
	if err := syscall.Kill(-cmd.Process.Pid, sig); err != nil {
		_ = cmd.Process.Signal(sig)
	}
}

// readTail returns the last n bytes of a file: enough stderr to explain a
// failure in an API response without storing megabytes in the job row.
func readTail(path string, n int64) string {
	f, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		return ""
	}
	off := st.Size() - n
	if off < 0 {
		off = 0
	}
	b := make([]byte, st.Size()-off)
	_, _ = f.ReadAt(b, off)
	return string(b)
}

// Identity is the SHA-256 of the solver binary: the solver_id that cache
// keys include (ADR-0002). Two builds of the same commit with different
// compiler flags are different solvers, and this is the only identifier
// that cannot be forgotten or mislabelled.
func Identity(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}
