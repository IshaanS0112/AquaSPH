# ADR-0003: Run the solver as a child process

**Status:** accepted

## Decision
The Go worker `fork/exec`s the `aquasph` binary per job and talks to it
through argv, a scenario file, JSON lines on stdout, an exit code, and
`SIGTERM`.

## Rejected alternative: cgo binding to `libaquasph_core`
- **Blast radius.** A solver segfault or `std::bad_alloc` inside cgo takes
  down the whole worker and every job on it. As a child process it is one
  failed job, marked `solver_crashed` and retried.
- **Cancellation.** A C++ loop inside cgo cannot be preempted by the Go
  runtime. A process can always be sent `SIGTERM` and then `SIGKILL`.
- **OpenMP inside a Go process** fights the Go scheduler for threads, and it
  makes `GOMAXPROCS` meaningless.
- **Build coupling.** cgo pulls a C++ toolchain into every Go build and
  test. With a process boundary, the backend tests run against a fake
  solver script in milliseconds.

## Cost
About 5 ms of process start-up per job. That is noise against solver
runtimes of seconds to minutes. The protocol is versioned implicitly by
`solver_id` (ADR-0002).
