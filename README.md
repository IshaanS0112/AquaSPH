<div align="center">

# AquaSPH

**A deterministic 3D fluid solver in C++17, run as a multi-tenant simulation platform in Go**

[![CI](https://github.com/IshaanS0112/AquaSPH/actions/workflows/ci.yml/badge.svg)](https://github.com/IshaanS0112/AquaSPH/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Go](https://img.shields.io/badge/Go-1.25-00ADD8.svg?logo=go&logoColor=white)](backend/)
[![PostgreSQL](https://img.shields.io/badge/PostgreSQL-16-4169E1.svg?logo=postgresql&logoColor=white)](backend/internal/db/migrations/)
[![Redis](https://img.shields.io/badge/Redis-7-DC382D.svg?logo=redis&logoColor=white)](docs/platform/TRD.md)
[![Docker](https://img.shields.io/badge/Docker-compose-2496ED.svg?logo=docker&logoColor=white)](docker-compose.yml)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/17)
[![OpenMP](https://img.shields.io/badge/OpenMP-parallel-EE4C2C.svg)](https://www.openmp.org/)
[![Tests](https://img.shields.io/badge/tests-89%20C%2B%2B%20%2B%2099%20Go%20%2B%207%20E2E-brightgreen.svg)](#testing)

Submit a fluid simulation over HTTP, watch it progress live, get
machine-readable physics back. Identical requests return in milliseconds
from a content-addressed cache, a worker killed mid-run loses nothing, and
fourteen scenarios run on an SPH solver whose output is bit-identical
across thread counts.

</div>

---

## The platform

```
 aquactl / HTTP ──► aquasph-api ──SQL──► Postgres 16 ◄──claim / lease / fence── aquasph-worker ×N ──fork/exec──► aquasph (C++)
   JSON, SSE          (Go)        │     jobs table = queue                        (Go)            ◄─JSON lines──   OpenMP solver
                                  └──► Redis 7: GCRA rate limit, progress pub/sub (optional; degrades, never fails)
```

The solver is a CPU-heavy batch program: a single run takes seconds to
many minutes on every core. The platform turns it into a service, and
most of the engineering went into the failure cases:

| Concern | How | Proof |
|---|---|---|
| **Durable queue** | The `jobs` table *is* the queue: `SELECT … FOR UPDATE SKIP LOCKED`, leases, and a fencing token on every post-claim write, so a worker that stalls past its lease cannot overwrite the next attempt ([ADR-0001](docs/platform/adr/0001-postgres-as-queue.md)) | 8 workers × 200 jobs complete each job exactly once; a zombie worker's writes are all rejected |
| **Per-tenant fairness** | The tenant row is locked before counting its running jobs, so a concurrency limit holds under races | Mutation-tested: without the lock, 8 of 8 simultaneous claims succeeded against a limit of 2 |
| **Crash recovery** | Expired leases are requeued with jittered backoff; the solver dies with its worker (`Pdeathsig`) | Live: a job survived two `kill -9`s of its worker and completed on attempt 3 ([audit trail](docs/platform/benchmarks.md#recovery-failure-injection-live-system)) |
| **Result cache** | Keyed on the canonical spec hash plus the SHA-256 of the solver binary. Sound *only* because the solver is bit-identical across thread counts ([ADR-0002](docs/platform/adr/0002-content-addressed-cache.md)) | A cache hit returns in 15 ms against 11 s for a fresh run, with byte-identical metrics |
| **Cancellation** | SIGTERM, then the solver finishes its step and writes partial metrics (exit 3), then SIGKILL after a grace period | 0.81 s from request to stopped, partial result kept |
| **Idempotency and quotas** | `Idempotency-Key` replay; queue quotas under an advisory lock; everything in one transaction | 10 concurrent retries create 1 job; 20 racing submits against a quota of 5 accept exactly 5 |
| **API hygiene** | API keys stored as hashes, constant-time compare; strict JSON (a typo such as `sim_tme` is a 400); RFC 9457 errors; keyset pagination; SSE with a Postgres fallback | An OpenAPI [spec](backend/api/openapi.yaml) that a test keeps in sync with the router |
| **Operations** | Prometheus metrics, structured logs with request IDs, graceful drain, health checks, a [runbook](docs/platform/runbook.md) | `docker compose stop worker` mid-job drains and *completes* the job |

API overhead on one shared 4-core VM (API, Postgres, Redis and the load
generator all on the same box): **2,852 req/s** reads at p99 20 ms,
**477 req/s** full transactional submits at p99 114 ms, zero errors
([benchmarks](docs/platform/benchmarks.md)). A 67 ms submit is 0.6% of
an 11 s low-quality run and noise against a 14-minute medium one, so the
solver, not the platform, is the bottleneck.

### Run it

New to the codebase? [`docs/GUIDE.md`](docs/GUIDE.md) covers opening it in an IDE, what every
folder is, and a walk through one job's path through the code.


```bash
make up                 # docker compose: postgres, redis, api, 2 workers
make demo-key           # prints an API key, once
export AQUASPH_API_KEY=aqk_...
cd backend && go build -o bin/ ./cmd/aquactl

bin/aquactl submit dam_break --time 0.3 --set /materials/0/viscosity=1.0 --watch
# [#####################.........]  71.3%  t=0.214/0.300 s  step 906  fluid 3864
# job 01a0e2e2-... completed (stable) after 11.052s

bin/aquactl submit dam_break --time 0.3 --set /materials/0/viscosity=1   # same physics
# job 01a0e2e2-... completed (cache hit: result of 01a0e2e2-...)

bin/aquactl sweep create dam_break --time 0.15 \
    --grid /materials/0/viscosity=0.5,1,5 --grid /numerics/xsph_epsilon=0,0.5
bin/aquactl sweep results <sweep-id> --metrics /density/max,/dynamics/max_speed
```

Design: [PRD](docs/platform/PRD.md) → [TRD](docs/platform/TRD.md) →
[ADRs](docs/platform/adr/) → code → [benchmarks](docs/platform/benchmarks.md)
→ [runbook](docs/platform/runbook.md). The rest of this README covers the
solver the platform runs.

---

## What this is

SPH is a Lagrangian, meshless method for simulating fluids: rather than
discretising space onto a fixed grid, the fluid is represented as
particles carrying their own mass, density and pressure, interacting
through a smoothing kernel.

AquaSPH implements the full pipeline from first principles, and then
builds a **scenario laboratory** on top of it. A dam break, a droplet
crown, a sloshing tank, a wave flume, a spillway and a flood are not six
demos — they are six JSON files composed from the same primitives:

```
Scenario = Domain + Materials + FluidRegions + Emitters + Sinks
         + Obstacles + ExternalForces + WaveGenerators
         + Numerics + Duration + Camera + Lighting + Render + Metrics
```

**The architectural test:** adding a new phenomenon must mean new JSON and
existing primitives, never new solver code. The fourteen scenarios in
`configs/scenarios/` contain no C++ between them.

```bash
./aquasph --list-scenarios
./aquasph --scenario droplet_impact --quality medium --metrics out.json
./aquasph_view --scenario waterfall --quality high --record frames/
```

---

## Credibility tiers

Results are only worth something if their claims match what the code can
actually do. Every scenario is assigned a tier, and that tier is printed
by every run, written into every metrics file, and repeated in
[`docs/gallery.md`](docs/gallery.md).

**Tier 1 — physically demonstrable.** Scales and phenomena that
weakly-compressible SPH genuinely resolves at achievable particle counts,
and that can be checked against experiment or an analytical result.

`dam_break` · `droplet_impact` · `sloshing_tank` · `obstacle_flow` ·
`controlled_wave_tank` · `spillway` · `container_fill` ·
`double_dam_break`

**Tier 2 — large-scale visual experiment.** Qualitatively informative,
visually compelling, and **not quantitatively predictive at these
resolutions.** Honest as demonstrations, dishonest as forecasts.

`flood` · `flash_flood` · `coastal_wave` · `waterfall` · `fountain` ·
`tsunami_pulse`

A Tier 2 flood is a visualisation of flow over terrain, **not a
hydrological prediction.** A Tier 2 tsunami pulse is a metre-scale
long-wave propagation experiment, **not a tsunami model** — nothing about
it transfers to any real coastline.

**Tier 3 — documented extension points, not implemented.** Erosion,
sediment transport, multiphase flow, foam and spray, ocean wave spectra,
moving rigid bodies, porous boundaries, river networks, mesh terrain. The
architecture leaves room; **no claims are made.**
[`docs/architecture.md`](docs/architecture.md) says where each would
attach and why none is a weekend's work.

![Contact sheet](docs/img/contact_sheet.png)

*All fourteen scenarios, `--quality low`, offline render on Mesa
`llvmpipe` (software rasteriser, no GPU) under Xvfb. Not real-time.*

---

## Physics

- Cubic spline (M4) kernel with an analytic gradient and 3D-correct
  normalisation
- Linked-cell neighbour search — O(N) via a 27-cell stencil, in
  compressed-sparse-row storage
- Weakly-compressible Tait EOS, per-material
- **Adaptive timestep** from the CFL / force / viscous triple, with
  deterministic reductions
- **XSPH** velocity correction (Monaghan 1989), applied to advection only
- **Surface tension** (Akinci et al. 2013): cohesion plus a curvature term
  from a colour-field normal
- **Boundary particles** (Akinci et al. 2012) with volume weighting —
  arbitrary static geometry through the same neighbour machinery
- Prescribed moving boundaries: wave paddles, pistons, gates
- Emitters, sinks, open and periodic domain faces
- Second-order predictor–corrector integration

**The velocity clamp is gone.** v1 capped every particle's speed because a
fixed timestep could not survive floor impact, and documented honestly
that the clamp then engaged continuously. Running v1 showed it was pinned
at exactly the clamp value for *every step of every run*, with average
density collapsing from 953 to 447 kg/m³. Two real bugs in the force law
were behind it — the pressure and viscous terms were being applied as
though divided by particle mass, and the viscous term had the wrong sign
so it added energy rather than removing it. Both are documented, both have
regression tests, and the measured before-and-after is in
[`docs/architecture.md`](docs/architecture.md).

---

## Rendering

Screen-space fluid rendering after van der Laan, Green & Sainz (I3D 2009).
**No mesh extraction, no marching cubes.** Sphere-impostor depth →
bilateral depth smoothing → normals from the smoothed depth → additive
thickness → Fresnel, refraction and Beer–Lambert absorption.

Three-point lighting fixed in world space, a matte floor with a scale grid
and a soft contact shadow, obstacles as opaque solids, and one restrained
palette across every scenario — so a contact sheet compares physics rather
than composition. No bloom, no lens flare, no depth of field.

Points mode is kept as a first-class diagnostic view, not a legacy path.
Full pipeline and its limitations: [`docs/rendering.md`](docs/rendering.md).

---

## Determinism

**Output is bit-identical across thread counts.** Not "close" — identical,
asserted by `memcmp` over the whole particle array after 40 steps at 1, 2,
4 and 8 threads, with surface tension, XSPH, boundary particles, emitters
and sinks all active.

That is harder than it was in v1, and deliberately kept:

- The **adaptive timestep** is a global reduction that feeds back into
  every later step, so a thread-dependent reduction would make the whole
  trajectory thread-dependent — silently. Reductions therefore use
  fixed-size chunks independent of thread count.
- **Emitters and sinks change the particle count during a run**, so
  ordering became a variable. Emission is ordered, removal is a stable
  compaction, and the linked cell's scatter pass is deliberately serial:
  a parallel scatter would reorder each cell, and a cell's order *is* the
  summation order of every neighbour loop that reads it.

See [`docs/experiments.md`](docs/experiments.md) for what the guarantee
does and does not cover.

---

## Measured performance

`dam_break`, Release, 4 physical cores. Full sweep and per-stage profile
in [`benchmarks/scaling_results.md`](benchmarks/scaling_results.md).

| Quality | Fluid | Boundary | 1T | 2T | 4T | Speedup |
|---|---:|---:|---:|---:|---:|---:|
| `low` | 3,864 | 13,176 | 18.6 ms | 9.7 ms | **5.8 ms** | 3.23× |
| `medium` | 21,525 | 39,314 | 107.7 ms | 55.3 ms | **30.9 ms** | 3.49× |
| `high` | 118,844 | 98,892 | 587.7 ms | 313.4 ms | **181.8 ms** | 3.23× |

`--profile` reports exclusive per-stage timings. At 218k particles and
four threads: forces 76%, density 19%, linked-cell build 2%.

The profile is also how the linked cell got rewritten — and it corrected
the prediction v1 made. v1 expected `build()` to become the bottleneck; it
is 4.3% of step time. What the profile actually showed is that a heap
bucket per cell made every neighbour query chase 27 pointers *inside* the
loops that are 95% of step time. Details and the before/after in the
benchmark file.

---

## Build

Requires CMake 3.16+, a C++17 compiler, and OpenMP. `glm`,
`nlohmann_json` and GoogleTest resolve via `find_package` if installed and
are fetched through CMake `FetchContent` otherwise — no vendored sources.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

<details>
<summary><b>macOS — additional OpenMP configuration</b></summary>

Apple Clang does not bundle OpenMP. Homebrew's `libomp` is keg-only, so it
is not on the default search path and CMake's `FindOpenMP` cannot locate
its flags and library from an `OpenMP_ROOT` hint alone. Pass the cache
variables explicitly:

```bash
brew install libomp
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DOpenMP_CXX_FLAGS="-Xpreprocessor -fopenmp -I$(brew --prefix libomp)/include" \
  -DOpenMP_CXX_LIB_NAMES="omp" \
  -DOpenMP_omp_LIBRARY="$(brew --prefix libomp)/lib/libomp.dylib"
```

Verified on Apple Silicon with AppleClang 21 and CMake 4.4.2.
</details>

<details>
<summary><b>Visualisation — optional, off by default</b></summary>

```bash
sudo apt install libglfw3-dev mesa-common-dev libgl1-mesa-dev \
  libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libx11-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release -DAQUASPH_BUILD_VISUALIZATION=ON
cmake --build build --parallel
```

The headless solver and the entire test suite link neither GL nor GLFW —
CI asserts that mechanically with `ldd` and `nm`, not by convention — so a
machine without GL development headers still builds and tests everything
else.

Headless frame capture works without a GPU, on Mesa's `llvmpipe`:

```bash
sudo apt install xvfb ffmpeg
xvfb-run -s "-screen 0 1600x900x24" ./build/aquasph_view \
    --scenario dam_break --quality high --record-headless --record frames/dam
scripts/make_video.sh frames/dam 30
```
</details>

---

## Usage

```bash
./aquasph --list-scenarios                       # names, tiers, descriptions
./aquasph --scenario dam_break                   # default: medium quality
./aquasph --scenario flood --quality high        # 6x the particles
./aquasph --scenario sloshing_tank --metrics out.json
./aquasph --scenario dam_break --profile         # per-stage timings
./aquasph --scenario coastal_wave --time 3.0 --threads 4
```

Every run reports `STABLE` / `UNSTABLE` with a matching exit code, so it
works directly as a CI check. `--metrics` writes particle counts, dt
statistics, the density envelope, the fraction of particles within 1% of
rest density, fluid volume, surge-front tracking, wave-probe records with
fitted amplitude and period, the linear-wavemaker prediction to compare
them against, inundation area, and the git revision that produced it.

Scripts:

```bash
scripts/run_scenarios.sh medium results/     # every scenario, STABLE/UNSTABLE table
scripts/benchmark.sh 40 dam_break            # throughput sweep
scripts/run_validation.sh                    # validation runs + comparison tables
scripts/make_gallery.sh medium               # clips and a contact sheet
```

---

## Testing

```bash
ctest --test-dir build --output-on-failure
```

**Solver:** 89 tests: the 81 below plus 8 for the process contract the
platform relies on (the `--progress-json` stream, SIGTERM leading to exit 3
with partial metrics, and JSON escaping). The 20 from v1 all still pass;
the only edit to them was dropping a removed constructor argument, and no
assertion was changed.

**Platform:** 99 Go tests plus a 7-scenario end-to-end suite, all against
real Postgres and Redis (the queue's correctness is SQL locking
behaviour, which no mock reproduces), all under the race detector:

```bash
cd backend && go test -race ./...     # needs Postgres + Redis; see internal/testutil
```

The end-to-end suite builds the binaries, starts an API and workers as
real processes on the real solver, then `SIGKILL`s and `SIGTERM`s
workers mid-job and checks the platform recovers. Four tests were
**mutation-checked**, meaning the code each one guards was removed and the
test was confirmed to fail: the tenant row lock (8 of 8 claims won against
a limit of 2), the quota lock (14 of 20 submissions accepted against a
quota of 5), the OpenAPI drift check, and cache-hit lineage.

The 81 solver tests from v2:

The 61 added in v2 cover, among others: the two force-law bugs (a viscous
term must oppose relative motion; a stored force must be a force and not
an acceleration), momentum conservation, XSPH direction, surface-tension
cohesion, the adaptive timestep's three limits and its NaN handling,
thread-count-independent reductions, **bit-identical output at 1/2/4/8
threads with emitters and sinks active**, boundary-volume scaling and its
measured shortfall at this resolution, **fluid not leaking through a floor
under impact**, hydrostatic equilibrium, shape containment and erosion,
`TimeSeries` shapes, scenario JSON round-tripping, wave-train fitting, and
linear wavemaker theory against its known limits, and the
**extensibility** cases that build scenarios entirely through the public
API in combinations no shipped scenario uses — if any of them had needed a
solver change, it would fail to compile. One of those found a real design
gap and got a face mode deleted; see below.

---

### One test that removed a feature

`Extensibility.PeriodicFacesWrapFluidInsteadOfLosingIt` was written to
exercise the `periodic` domain face, the one face mode no shipped scenario
used. It failed: a driven channel lost more than half its fluid within
half a second.

The cause was not a bug in the wrap. Wrapping a particle's *position* at a
face is four lines and does not produce a periodic domain — the neighbour
search computes separations directly, so a particle near one end of the
axis has no neighbours at the other and sees a free surface exactly where
the domain is meant to be continuous. A real periodic domain needs the
minimum-image convention threaded through the neighbour search and every
force loop: a change to the solver, not a face mode.

**The mode was removed rather than shipped.** Asking for `"periodic"` now
prints that explanation instead of silently substituting a wall, and the
test that caught it was rewritten to pin that behaviour. An unused code
path is an untested one, and a half-working feature is worse than an
honest extension point.

## Documentation

| Document | Contents |
|---|---|
| [`docs/GUIDE.md`](docs/GUIDE.md) | Start here: IDE setup, a map of every folder, one job traced through the code |
| [`docs/platform/PRD.md`](docs/platform/PRD.md) | The platform's users, requirements, explicit non-goals, and success criteria |
| [`docs/platform/TRD.md`](docs/platform/TRD.md) | Topology, job state machine, queue semantics, cache keying, data model, solver protocol, failure modes |
| [`docs/platform/adr/`](docs/platform/adr/) | Four decisions with their rejected alternatives: Postgres as the queue, the content-addressed cache, process isolation for the solver, hashed API keys |
| [`docs/platform/benchmarks.md`](docs/platform/benchmarks.md) | Measured API throughput and latency, cache and cancellation timings, and a live recovery trace |
| [`docs/platform/runbook.md`](docs/platform/runbook.md) | Worker sizing, alerts, every failure mode and what to do, tenant and key management, full configuration reference |
| [`backend/api/openapi.yaml`](backend/api/openapi.yaml) | The HTTP API (OpenAPI 3.1) |
| [`docs/architecture.md`](docs/architecture.md) | Module map, parallelisation, seven documented bugs with how each was found, the Phase 0 before/after, boundary resolution measured, the dynamic-particle audit, known physical approximations, Tier 3 extension points |
| [`docs/scenarios.md`](docs/scenarios.md) | How to author a scenario. Every primitive, every field, a worked example |
| [`docs/gallery.md`](docs/gallery.md) | Each scenario: tier, physics demonstrated, parameters, and one honest sentence on where the model is approximate |
| [`docs/rendering.md`](docs/rendering.md) | The SSFR pipeline pass by pass, art direction, and known rendering limitations |
| [`docs/validation.md`](docs/validation.md) | Dam break vs the Ritter analytical solution, sloshing period vs shallow-water theory, wave-generator fidelity |
| [`docs/experiments.md`](docs/experiments.md) | Reproducibility: what a run records, what determinism does and does not promise, how to re-derive every published number |
| [`benchmarks/scaling_results.md`](benchmarks/scaling_results.md) | Measured throughput, scaling, and the per-stage profile |

`docs/architecture.md` is cumulative on purpose. The account of what was
wrong in v1 — a 2D normalisation constant used in 3D, a discontinuous
kernel, a rotationally-variant viscosity, a 35× CFL violation, tensile
instability, a floor-impact spike — is kept exactly as written, and the
two v2 force-law bugs are added to it rather than replacing it.

---

## License

Released under the [MIT License](LICENSE).
