# Project guide: open it, find your way around, understand it

Read this side by side with the code. In VS Code: open this file, press `Ctrl+K V`.

## 1. Get the project onto your computer

The whole project comes as one file, `aquasph-platform.bundle` (a git repository in a single file).

If you already have an AquaSPH folder (a git clone), run this inside it:
```bash
git fetch /path/to/aquasph-platform.bundle claude/aquasph-scenario-lab-97pe5k:claude/aquasph-scenario-lab-97pe5k
git checkout claude/aquasph-scenario-lab-97pe5k
```

If you are starting fresh:
```bash
git clone -b claude/aquasph-scenario-lab-97pe5k /path/to/aquasph-platform.bundle AquaSPH
cd AquaSPH
git remote set-url origin https://github.com/IshaanS0112/AquaSPH.git
```

Push it to GitHub with your own login:
```bash
git push -u origin claude/aquasph-scenario-lab-97pe5k
```

## 2. What to install

| Your OS | What to do |
|---|---|
| Windows | Editing works natively. To build and run, use WSL2: `wsl --install -d Ubuntu-24.04` in an admin PowerShell. Keep the project inside WSL (`~/AquaSPH`), not under `/mnt/c`, where builds are about 10x slower. |
| Ubuntu / WSL | `sudo apt install build-essential cmake git`, then Go 1.25 from go.dev (apt's Go is too old) |
| macOS | `brew install cmake libomp go` (see README, Build, for the OpenMP flags) |
| All | Docker Desktop, for `make up` and the test databases (turn on WSL integration on Windows) |

Why Linux or macOS for running: the solver uses OpenMP, and the worker stops solver runs with Unix
signals. The Go code compiles on Windows, so the IDE shows no errors there, but running jobs needs
WSL2 or Docker.

## 3. Open it in VS Code

1. Windows: install the **WSL** extension, then in the Ubuntu terminal run `cd ~/AquaSPH && code .`.
   On macOS/Linux, just run `code .` in the project folder.
2. Install these extensions: **C/C++**, **CMake Tools**, **Go**, **Docker**, **YAML**.
3. C++: CMake Tools asks for a kit. Pick GCC, then run *CMake: Configure* and *CMake: Build*.
4. Go: nothing to do. `go.work` at the root points the Go extension at the module in `backend/`.
5. Read docs next to code: open any `.md` file and press `Ctrl+K V`.

CLion (C++) and GoLand (Go) also work: open the root folder in either.

Comments in the code are kept short: one line saying what the code does, and why when that is not
obvious. The long explanations live in `docs/`.

## 4. What each folder is

The project has two halves: the solver (C++, the physics) and the platform (Go, the backend service
that runs the solver for users).

```
AquaSPH/
├── src/                  C++ solver
├── tests/                solver tests
├── configs/scenarios/    the 14 simulations, as JSON
├── backend/              Go platform
├── docs/                 design documents (you are here)
├── scripts/              helper scripts
├── benchmarks/           solver speed results
├── validation/           reference experiment data
├── results/validation/   saved validation outputs
├── Dockerfile            builds one image with everything
├── docker-compose.yml    runs the full stack: postgres, redis, api, 2 workers
├── Makefile              shortcuts: make test, make up, make e2e ...
├── CMakeLists.txt        C++ build definition
├── go.work               tells Go tools where the Go module is
└── .github/workflows/    CI: runs every test on each push
```

### src/ (the solver)

| Path | What it does |
|---|---|
| `main.cpp` | The `aquasph` program. Loads a scenario, runs it, prints progress, writes `metrics.json`. The Go worker runs this. |
| `render_main.cpp` | `aquasph_view`, the optional 3D viewer (needs OpenGL) |
| `core/Particle.hpp` | One fluid particle: position, velocity, density, pressure, mass |
| `core/SPHKernel.*` | The smoothing function W(r, h) that weights neighbours by distance |
| `core/DensityPressure.*` | Step 1 of physics: density from neighbours, pressure from density |
| `core/ForceCompute.*` | Step 2: pressure, viscosity, surface-tension and wall forces |
| `core/Integrator.*` | Step 3: moves particles one timestep and keeps them inside walls |
| `core/TimeStep.*` | Picks a safe timestep each step |
| `core/BoundaryVolume.*` | How walls (made of particles) push back on the fluid |
| `core/Material.hpp` | Fluid properties: density, viscosity, surface tension |
| `core/ParallelReduce.hpp` | Multi-threaded max/sum in a fixed order: why results are identical at any thread count |
| `spatial/LinkedCell.*` | Neighbour search: a grid, so each particle only checks nearby cells |
| `scene/Scenario.*` | The data model of a simulation: domain, materials, water regions, emitters, obstacles, waves |
| `scene/ScenarioLoader.*` | Turns a JSON file into a Scenario and rejects bad input |
| `scene/Simulation.*` | Owns all particles; `step()` runs the whole pipeline once |
| `scene/Shapes.*`, `TimeSeries.*`, `Quality.hpp` | Geometry, motion over time, low/medium/high presets |
| `metrics/Metrics.*` | Measurements (surge front, waves, volume); writes `metrics.json` |
| `render/` | Viewer only: camera, shaders, water-surface renderer, PNG recording |
| `benchmark/PerfTimer.hpp` | Timing helper |

### tests/

89 GoogleTest tests for the solver: kernel, forces, timestep, determinism, walls, scenarios.
`tests/cli/` checks the program's exit codes and its progress output.

### backend/ (the platform)

| Path | What it does |
|---|---|
| `cmd/aquasph-api` | The HTTP server |
| `cmd/aquasph-worker` | Takes jobs from the queue and runs the solver |
| `cmd/aquasph-admin` | Operator tool: tenants, API keys, workers, queue |
| `cmd/aquactl` | The user CLI: submit, watch, cancel, results, sweeps |
| `cmd/aquasph-loadgen` | Load tester |
| `internal/api` | Routes and handlers. `middleware.go` does auth and rate limiting; `sse.go` streams live progress |
| `internal/service` | Submission rules: validate, cache lookup, quotas, idempotency, sweeps |
| `internal/queue` | The job queue: claim, heartbeat, complete, retry, and the reaper for dead workers |
| `internal/worker` | Worker loop: claim, cache check, run the solver, save results |
| `internal/solver` | Starts the C++ program, reads its progress, stops or kills it (`*_windows.go` / `*_unix.go` are the per-OS parts) |
| `internal/store` | All SQL for tenants, keys, jobs, sweeps, workers |
| `internal/db` | DB connection and migrations. `migrations/0001_init.sql` is the full schema |
| `internal/scenario` | Scenario catalogue, `--set /path=value` overrides, the cache-key hash, sweep grids |
| `internal/auth` | API keys (only hashes are stored) |
| `internal/ratelimit` | Redis rate limiter |
| `internal/events` | Redis pub/sub for live progress |
| `internal/artifacts` | Stores output files (metrics.json, logs) |
| `internal/obs` | Logging and Prometheus metrics |
| `internal/domain` | Shared types: Job, Tenant, job states, error codes |
| `internal/config` | Reads the `AQUASPH_*` environment variables |
| `internal/client` | Go API client used by aquactl, loadgen and the tests |
| `internal/app`, `ids`, `testutil` | Startup helpers, ID generation, test database helpers |
| `api/openapi.yaml` | The API specification |
| `e2e/` | End-to-end test: real processes, workers killed mid-job |

### docs/

| File | Read it for |
|---|---|
| `platform/PRD.md`, then `TRD.md`, then `adr/` | What the backend does, how it works, and why each big decision was made |
| `platform/benchmarks.md` | Measured performance |
| `platform/runbook.md` | How to operate it: failures, alerts, settings |
| `platform/verification.md` | What was tested on the live system, and the bugs that found |
| `architecture.md` | Solver design, and the physics bugs found and fixed |
| `scenarios.md`, `gallery.md`, `validation.md`, `rendering.md`, `experiments.md` | Writing scenarios, what each one shows, physics checks, the renderer, reproducibility |

## 5. Follow one job through the code

The fastest way to understand everything. Open these in order:

1. `backend/cmd/aquactl/main.go` (the `submit` case) sends `POST /v1/jobs`.
2. `backend/internal/api/middleware.go` (`authenticate`) checks the API key, then the rate limit.
3. `backend/internal/api/jobs.go` (`createJob`) hands the request to the service.
4. `backend/internal/service/service.go` (`SubmitJob`) applies overrides and computes the hash, then
   runs the cache check, the quota check and the insert in one database transaction.
5. `backend/internal/queue/queue.go` (`claimOnce`) is where a worker grabs the job with
   `FOR UPDATE SKIP LOCKED`. Know this function well.
6. `backend/internal/worker/worker.go` (`execute`) writes `scenario.json`, starts heartbeats and
   runs the solver.
7. `backend/internal/solver/solver.go` (`Run`) starts `aquasph --progress-json` and reads its output
   line by line.
8. `src/main.cpp`, then `src/scene/Simulation.cpp` (`step`): neighbour grid, density and pressure,
   forces, timestep, move particles.
9. Progress goes through Redis to `backend/internal/api/sse.go`, which drives the progress bar.
10. `worker.go` (`record`) saves the files, then calls `queue.Complete`. That write only succeeds
    while this worker still holds the job's lease.

## 6. Run it

```bash
# Solver only
make solver test                              # build and run the 89 tests
./build/aquasph --list-scenarios
./build/aquasph --scenario dam_break --quality low --time 0.5

# The whole platform (Docker)
make up && make demo-key                      # copy the printed key
export AQUASPH_API_KEY=aqk_...
cd backend && go build -o bin/ ./cmd/aquactl
bin/aquactl submit dam_break --time 0.3 --watch

# Go tests (they need Postgres and Redis)
docker run -d -p 5432:5432 -e POSTGRES_HOST_AUTH_METHOD=trust postgres:16
docker run -d -p 6379:6379 redis:7
cd backend && go test ./...
make e2e                                      # full system test, about 1 minute
```

## 7. Before an interview, be able to explain

1. Why the queue is a Postgres table, not RabbitMQ or Kafka (`docs/platform/adr/0001`)
2. What `SKIP LOCKED` does, and why the tenant row is locked before counting (`queue.go`)
3. What a lease and a fencing token are, and what happens when a worker dies (`TRD.md`, section 3)
4. Why caching results is safe only because the solver gives identical output at any thread count
   (`adr/0002`)
5. Why the solver runs as a separate process instead of being linked into Go (`adr/0003`)
