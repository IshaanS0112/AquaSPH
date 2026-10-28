# Verification record

What was checked on the **running system** (real processes, the real
solver, real Postgres and Redis, then the real Docker image and Compose
stack), as opposed to what the test suites cover. `backend/e2e` automates
most of it so CI repeats it on every change.

## What was run

| Check | Result |
|---|---|
| `aquactl submit dam_break --watch` against API + 2 workers | Live progress over SSE; completed STABLE in 11.05 s |
| Same request again, and again with `1` instead of `1.0` | Cache hit in 15 ms; canonical hashing makes the spellings equal; result byte-identical |
| A different override value | Cache miss, queued: the hash is not over-eager |
| 6-job sweep (viscosity × XSPH) | All completed across both workers; results table extracted by JSON Pointer |
| Cancel a running job | Terminal in 0.81 s; partial metrics with status CANCELLED |
| `kill -9` the worker running a job, twice | Requeued via `lease_expired` both times; completed STABLE on attempt 3 |
| Solver state after its worker is SIGKILLed, sampled every 20 ms | Zombie (dead) at t+0 ms: `Pdeathsig` works |
| SIGTERM all processes | Drain messages logged; all four workers marked stopped in the database |
| `docker build` | 136 MB image, non-root, static Go binaries; a real solver run inside it |
| `docker compose up --wait` + README quick start | All 5 containers healthy; a real job completed |
| `docker compose stop worker` mid-job | Job drained to completion (attempt 1) in 4 s |
| CI container step, executed verbatim | Passed |
| Load test, 3 modes × 20 s × 32 clients | 81,705 requests, 0 errors ([benchmarks.md](benchmarks.md)) |
| Full Go suite with `AQUASPH_REQUIRE_INTEGRATION=1 AQUASPH_REQUIRE_SOLVER=1 -race` | 99 tests + 7 E2E scenarios, 0 skipped, 0 failed |
| Solver: `ctest` and the CI `-Wall -Wextra -Werror` build | 89/89; warning-free |

## Bugs this found that the tests had not

Every one of these is fixed, and each now has a test.

1. **The final `done` event was sometimes lost** (about 1 run in 6).
   `cmd.StdoutPipe()` is closed by `cmd.Wait()` when the process exits,
   so waiting before the reader hit EOF dropped the last lines. The
   supervisor now owns its pipes. Found by an intermittent test failure
   that was chased down rather than retried.
2. **A cache hit could name the wrong source.** A hit on a result that
   had already been hit reported the previous *hit* as its source, not
   the job that computed it. The lookup now prefers the original. Seen
   in `aquactl` output on the live system.
3. **A worker ran without metrics.** Started while its metrics port was
   taken, it logged the error and kept running jobs, invisible to
   monitoring. Both binaries now bind their ports first and exit on
   conflict. Found because a botched restart left two worker
   generations running side by side.
4. **The Docker build could not work.** The solver stage did not copy
   `configs/`, which CMake's configure step copies. Found by building
   the image rather than just writing the Dockerfile.
5. **Orphaned solvers linger as zombies in containers.** Not a
   correctness bug (they are dead, and use no CPU), but they accumulate
   under a PID 1 that does not reap. Workers now run with `init: true`.

## Things that looked like bugs and were not

- **"Orphaned solver still running"** 300 ms after its worker was killed.
  `kill -0` succeeds on a zombie. Sampling `/proc/<pid>/status` showed
  state `Z` from the first instant.
- **"Integration tests pass"** after a container restart, when in fact
  they were *skipped* because Postgres and Redis had not come back.
  Locally a missing service means skip; CI sets
  `AQUASPH_REQUIRE_INTEGRATION=1` so the same situation fails the build.
  The counts in the README come from a run with that flag set.

## Environment notes

The sandbox this was built in blocks Debian's package mirrors and
intercepts TLS. The image was therefore built from a mechanically derived
copy of `Dockerfile` that trusts the proxy's CA and uses HTTPS apt
sources. That is the only difference, and the repository's `Dockerfile`
is the unmodified original. Docker Hub rate-limited anonymous pulls, so
the daemon used Google's public mirror (`mirror.gcr.io`), which serves
the same images.
