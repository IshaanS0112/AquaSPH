# AquaSPH

A 3D Smoothed Particle Hydrodynamics (SPH) fluid simulation engine in
C++17, with OpenMP-parallelized physics and an optional real-time OpenGL
viewer -- Phase 0 (core solver) + Phase 1 (CPU parallelism) + Phase 1.5
(visualization) of a larger project (CUDA planned for V2).

SPH is a Lagrangian, meshless numerical method for simulating fluids:
instead of a fixed grid, the fluid is represented as particles that
carry their own mass, density, and pressure, and interact with nearby
particles through a smoothing kernel. It's standard in graduate
computational physics / CFD coursework and production VFX tools; this
project implements it from the ground up as an undergraduate systems/HPC
project.

## What's implemented

**Phase 0 -- core solver:**
- Cubic spline (M4) smoothing kernel with gradient, 3D-correct
  normalization
- Linked-cell spatial hashing for O(N) neighbor search (vs. O(N^2)
  naive all-pairs)
- SPH density estimation + Tait equation of state (weakly-compressible
  water)
- Force computation: pressure gradient, viscosity, gravity
- 2nd-order predictor-corrector time integration
- JSON-configurable simulation parameters
- Dam-break scenario, stable for 1,200+ timesteps at 8,000 particles
- 20 unit tests (GoogleTest) covering the kernel, density/EOS, neighbor
  search, and integrator

**Phase 1 -- OpenMP parallelism:**
- `computeDensityPressure`, `computeForces`, and all three
  integrator loops parallelized over particles (`#pragma omp parallel
  for`), with per-thread neighbor buffers to avoid data races
- `--threads N` CLI flag to set thread count at runtime
  (`omp_set_num_threads`)
- Determinism verified: identical physics output at 1/2/4 threads
  (parallelism changes wall-clock time, not results)
- Measured speedup: ~3.0-3.5x at 4 threads (this dev machine's physical
  core count) across 5,832-50,653 particles -- see
  [`benchmarks/scaling_results.md`](benchmarks/scaling_results.md) for
  the full sweep, including the (expected, explained) flat/negative
  result at 8 threads on 4 physical cores

**Phase 1.5 -- OpenGL/GLFW viewer:**
- A second executable, `aquasph_view`, renders the live simulation in
  real time (GL_POINTS particles colored by speed) instead of just
  benchmarking it -- opt in with `-DAQUASPH_BUILD_VISUALIZATION=ON`
- Runs the exact same physics as `aquasph` (shared `core/DamBreakInit.*`
  and the same four-call step sequence each frame), so what's on screen
  is provably the same simulation, not a simplified stand-in
- Mouse-orbit camera (left-drag to orbit, scroll to zoom), Esc to quit
- Hand-rolled OpenGL 3.3 core function loader (`src/render/GLLoader.*`)
  instead of GLAD/GLEW -- see `docs/architecture.md` for why that's the
  right call here, not just a smaller dependency
- Compile/link-verified end to end against a real, locally-built GLFW
  (zero warnings under `-Wall -Wextra`); actually rendering a frame
  needs a real display, which this project's own dev sandbox doesn't
  have -- see `docs/architecture.md`, "Phase 1.5", for exactly what that
  investigation found and how far verification could go without one

See [`docs/architecture.md`](docs/architecture.md) for the module
breakdown and, more importantly, for **three physics/math bugs found in
the original project spec** (a mislabeled kernel normalization constant,
a discontinuous kernel polynomial, and a dimensionally-invalid viscosity
formula) plus two real numerical-stability failures found only by
actually running the simulation (SPH tensile instability at the free
surface, and a floor-impact force spike) -- what caused each one, and
exactly what was changed and why.

## Build

Requires CMake 3.16+, a C++17 compiler, and OpenMP (bundled with
GCC/Clang). `glm`, `nlohmann_json`, and GoogleTest are fetched
automatically via CMake `FetchContent` if not already installed on your
system.

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j4
```

## Run

```bash
./aquasph                                   # default scenario (configs/default.json)
./aquasph --particles 20000 --steps 500     # override particle count / step count
./aquasph --threads 4                       # override OpenMP thread count (default: omp_get_max_threads())
./aquasph --config ../configs/default.json  # explicit config path
./aquasph --quiet                           # suppress periodic progress lines
```

Periodic output includes step timing, FPS, min/avg/max SPH density, and
an out-of-bounds/NaN particle count; the run ends with a STABLE/UNSTABLE
verdict (and a matching process exit code) based on whether any particle
went non-finite or left the domain. The final summary also reports the
active thread count. Run `nproc` to see how many physical cores your
machine has -- that's generally the most effective `--threads` value,
not necessarily the highest one available (see
[`benchmarks/scaling_results.md`](benchmarks/scaling_results.md) for why
oversubscribing past physical core count doesn't help).

## Visualize (Phase 1.5, optional)

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DAQUASPH_BUILD_VISUALIZATION=ON ..
make -j4
./src/aquasph_view                      # default scenario, real-time render
./src/aquasph_view --particles 20000    # denser block
```

Left-click-drag to orbit the camera, scroll to zoom, Esc to quit.
Particles are colored by speed (blue = still, white = fast-moving --
e.g. the initial floor impact). On Debian/Ubuntu, GLFW's X11 backend
needs a few dev packages this project's own build environment happened
to be missing -- `docs/architecture.md` ("Phase 1.5") has the exact
`apt install` line and the full investigation behind it.

## Test

```bash
cd build
ctest --output-on-failure
```

20/20 tests passing: kernel normalization (verified by numerical
integration) and gradient symmetry, Tait EOS sanity checks, linked-cell
neighbor-count correctness (interior vs. boundary cells), and integrator
checks (parabolic free-fall trajectory, wall containment, energy bound).

## Project layout

```
src/core/       Particle struct, SPH kernel, density/EOS, forces, integrator, dam-break init
src/spatial/    Linked-cell neighbor search
src/io/         JSON config loading
src/benchmark/  Timing utility
src/render/     OpenGL loader, shader, camera, particle renderer (Phase 1.5)
src/main.cpp        Headless benchmark entry point (-> aquasph)
src/render_main.cpp  Real-time viewer entry point (-> aquasph_view, opt-in)
configs/        Simulation parameters (JSON)
tests/          GoogleTest unit tests
benchmarks/     Measured throughput results
docs/           Architecture + design-decision writeup
```

## Roadmap

- **Phase 1 (done):** OpenMP parallelization of the density/force/
  integrator loops -- adaptive timestepping remains a V2 candidate
- **Phase 1.5 (done):** OpenGL + GLFW real-time visualization
  (`aquasph_view`) -- compile/link-verified against a real GLFW; actual
  on-screen rendering needs a real display to check, which this
  project's own dev sandbox doesn't have (see `docs/architecture.md`)
- **V2:** CUDA acceleration, adaptive timestepping

## License

MIT (or your preference -- add a LICENSE file before making the repo
public if you want this enforced).
