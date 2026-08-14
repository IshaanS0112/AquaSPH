# AquaSPH -- Architecture and Design Decisions

## Overview

A CPU-based 3D Smoothed Particle Hydrodynamics fluid solver with the hot
per-particle loops parallelized via OpenMP. Given a dam-break initial
condition -- a block of particles in a box under gravity -- it computes
density, pressure, and forces each timestep through kernel-weighted
neighbor sums, integrates with a predictor-corrector scheme, and reports
throughput and stability.

The physics was built and validated single-threaded before any
parallelization was introduced, and the parallelization was then verified
to leave results bit-identical. See "Determinism check" in
[`benchmarks/scaling_results.md`](../benchmarks/scaling_results.md) for
how that was measured.

## Module map

```
core/Particle.hpp        Particle struct (AoS layout)
core/Constants.hpp        Shared physical/numerical constants
core/SPHKernel.*           Cubic spline kernel W(r,h) and its gradient
core/DensityPressure.*     rho_i = sum m_j W_ij; Tait EOS pressure
core/ForceCompute.*        Pressure gradient + viscosity + gravity
core/Integrator.*          Predictor-corrector time integration + walls
spatial/LinkedCell.*       Uniform-grid neighbor search (27-cell stencil)
io/ConfigLoader.*          JSON config with safe fallback to defaults
benchmark/PerfTimer.hpp    Wall-clock timing utility
main.cpp                   Dam-break init, sim loop, benchmark reporting
```

Data flows one direction each step: `LinkedCell::build` -> 
`computeDensityPressure` -> `computeForces` -> `Integrator::step`
(which internally re-invokes `computeForces` once, for the
predictor-corrector's half-step). No module reaches back into an
earlier stage's internals -- `Integrator` doesn't even link against
`ForceCompute` or `LinkedCell` directly, it takes a
`std::function<void(std::vector<Particle>&)>` callback from `main.cpp`,
which is what keeps it unit-testable in isolation (see
`tests/test_integrator.cpp`, which drives it with a trivial
gravity-only callback and zero neighbors).

## Parallelization strategy

Four loops dominate per-step cost: the neighbor-sum loop inside
`computeDensityPressure`, the neighbor-sum loop inside `computeForces`,
and the three particle-local loops inside
`PredictorCorrectorIntegrator::step`. All four are now threaded with
`#pragma omp parallel for schedule(static)` (or, for the two
neighbor-search loops, `#pragma omp parallel` with a per-thread
`neighbors` buffer wrapping the `#pragma omp for`, since each thread
needs its own scratch vector to call `LinkedCell::getNeighbors` into --
The original serial code had one shared `neighbors` vector reused every iteration
via `.clear()`, which is only safe single-threaded).

Each of the four loops is safe to parallelize over particles because
every iteration `i` writes only `particles[i]`'s own field(s) --
density+pressure in one loop, force in another, velocity/position in
the integrator's -- and only *reads* (never mutates) neighboring
particles' data. No two iterations ever write the same memory, so
there's nothing to race on and no locking/atomics needed. This is the
textbook "embarrassingly parallel" case, which is exactly why these
loops (and not, say, a hypothetical global reduction) were the primary
parallelization target.

`LinkedCell::build` stays serial by contrast, and deliberately so: it
writes into shared per-cell buckets (`std::vector<int>` per cell), and
multiple particles from different threads can legitimately land in the
same cell, so a naive parallel `push_back()` there **would** race. See
the comment in `spatial/LinkedCell.cpp` for the counting-sort
restructure that would be needed to parallelize it correctly, and why
it wasn't the first thing threaded (`build()` is O(N) with cheap
per-particle work; the four loops above are O(N * neighbor count), with
far more work per particle -- higher payoff for the same parallelization
effort).

Correctness was checked by more than "it compiles and doesn't crash":
the same dam-break run was executed at 1, 2, and 4 threads and compared
checkpoint-by-checkpoint (density min/avg/max, min Y position, max
speed) -- all identical across thread counts. Throughput was checked
with a full sweep at 1/2/4/8 threads across four particle counts; see
[`benchmarks/scaling_results.md`](../benchmarks/scaling_results.md) for
the numbers, the ~3.0-3.5x speedup at 4 threads (this machine's physical
core count), and why 8 threads doesn't help further here.

## AoS vs SoA

`Particle` is Array-of-Structures (each particle's position, velocity,
force, density, pressure, mass live together in one struct). This is
the simpler layout, and a deliberate choice: establish correctness
first, optimize against measurements rather than priors. SoA
(separate `vector<vec3>` for position, separate `vector<float>` for
density, etc.) is generally more cache- and SIMD-friendly for the
density/force loops, since a loop touching only position and density
doesn't have to pull mass/pressure/force into cache alongside it. The
cost is every "particle" access becomes several parallel-array index
operations instead of one struct dereference -- more invasive, harder to
read, and premature before profiling shows AoS is actually the
bottleneck (vs. the O(N) neighbor search itself, or lock contention once
OpenMP is introduced).

The benchmark sweep gives a first, imperfect data point on this:
parallel efficiency at 4 threads declines from ~87% at 5,832 particles
to ~76% at 50,653 (see
[`benchmarks/scaling_results.md`](../benchmarks/scaling_results.md)),
the opposite of the usual "overhead amortizes better at scale" pattern.
A plausible read is that AoS is costing more cache bandwidth than SoA
would at large N -- each thread pulls all six fields of a `Particle` into
cache even when a given loop needs two or three -- and four threads
sharing fixed memory bandwidth would show up as exactly this kind of
efficiency drop. That's a hypothesis consistent with the measured
numbers, not a confirmed diagnosis (no `perf stat`/cache-miss profiling
has actually been run yet) -- worth confirming with a profiler before
committing to an SoA rewrite.

## Correctness problems found in the initial formulation

This section records where the initial formulation was wrong and why.
Repeating the reference formulas as given would have produced a simulation
that ran and looked plausible while being incorrect - these are the errors
that had to be found before any result meant anything.

### 1. Kernel normalization constant was the 2D value, and the piecewise polynomial was discontinuous

The spec's kernel formula quoted `sigma = 10/(7*pi*h^2)` labeled "3D" --
that constant has units of 1/length^2, so it's actually the 2D
normalization; using it in 3D silently biases every density estimate.
Separately, its two polynomial pieces (breakpoint at `r=h/2`, cutoff at
`r=h`) don't agree at the breakpoint: 0.4792 vs 0.5625 -- a genuine
discontinuity, not just a units error. This wasn't caught by inspection;
it showed up as a failing `SPHKernel.MonotonicallyDecreasing` unit test
(a kernel jumping from 370 to 409 partway through its support is not
monotonic) during initial testing.

The root cause: the formula appears to be the *real* standard cubic
spline (whose natural support is `[0, 2h]`, breakpoint at `h`) with its
breakpoints simply halved to `h/2` and `h`, without rescaling the
polynomial to match -- i.e. a substitution error. `SPHKernel.hpp` derives
the correct fix: substitute `Q = 2*(r/h)` into the genuine standard
kernel, which gives a continuous (in fact C1-continuous) piecewise cubic
and a clean closed-form 3D normalization constant, `sigma = 8/(pi*h^3)`
-- verified both by a from-scratch integral and numerically (Riemann sum)
in `tests/test_kernel.cpp`.

### 2. The viscosity force formula was dimensionally ill-defined

The spec's viscosity term is `mu * sum_j m_j * (v_j - v_i) * gradW_ij / rho_j`
-- a vector `(v_j - v_i)` multiplied by a vector `gradW_ij`, with no
dot/cross specified. Taken component-wise, that's not rotationally
invariant: the simulation's behavior would depend on how the domain axes
happen to be oriented, which is physically wrong for an isotropic fluid.
`ForceCompute.cpp` instead uses the standard SPH discretization of the
velocity Laplacian (the same trick behind Monaghan's and Morris et al.'s
viscosity terms): fold `gradW` into a scalar via
`dot(r_ij, gradW_ij) / |r_ij|^2`, which is rotation-invariant and reuses
the existing kernel gradient (no second kernel needed). `mu` remains a
tunable damping coefficient, exactly as the spec's own notes describe it
("viscosity is artificial").

### 3. c0 = 1400 m/s + dt = 0.001s violates CFL by over 30x

The spec sets the artificial speed of sound to water's real value
(~1400 m/s) and a fixed `dt = 0.001s`. The CFL stability condition for
this kind of explicit weakly-compressible SPH is roughly
`dt <~ 0.4*h/c0`; with `h=0.1`, that puts the safe ceiling at
`dt <~ 2.86e-5s` -- about 35x smaller than the spec's `dt`. Since
`dt` is fixed in the current solver (adaptive stepping is future work),
the only lever is `c0`: solving `0.4*h/c0 = dt` for the *largest*
CFL-safe `c0` at `dt=0.001, h=0.1` gives `c0 <~ 40`. The shipped default
(`c0=25`) sits comfortably under that ceiling, trading a softer,
more-compressible EOS for a fixed, spec-mandated timestep.

## Instabilities found only at runtime

Getting the physics formulas right wasn't sufficient -- running the full
6,800+ particle, 1,000+ step scenario surfaced two more failure modes
that no amount of formula-checking alone would have caught. Documented
here in the order they were found, since each one masked the next.

### Particle mass must be derived from lattice spacing, not read from config

`p.mass = cfg.mass` (the spec's literal "1.0, uniform, normalized")
produced an initial SPH density of ~7,600 kg/m^3 against the target
1,000 -- before a single timestep ran. SPH density is a mass-weighted
kernel sum, `rho_i = sum_j m_j * W(r_ij, h)`; for a uniform lattice at
spacing `s` to average out to `rho0`, each particle needs to "own"
roughly one cell of fluid: `m = rho0 * s^3`. With `h=0.1`, `spacing=h/2`,
that's `m ~ 0.125`, not `1.0`. `main.cpp::initializeDamBreak` now derives
mass this way instead of reading a fixed config value. (`cfg.mass`
remains in `Config` for a hypothetical non-lattice initializer, e.g. a
future particle emitter, that would need a literal per-particle mass.)

### SPH tensile instability at the free surface, then a floor-impact force spike -- two layers of the same "explicit + fixed dt + stiff force law" problem

With mass fixed, initial density was correct (~950 avg, close to
`rho0`), but the simulation still diverged, in two distinct stages:

1. **Tensile instability.** Particles near the block's outer faces (a
   large fraction of them, for a small block) register *below* rest
   density purely from having an incomplete kernel neighborhood -- no
   fluid beyond the surface to contribute. Tait pressure for
   below-rest density is negative, and because the SPH pressure force is
   symmetric in `P_i + P_j`, negative pressure flips it from repulsive to
   *attractive*. Surface particles spuriously clump, raising local
   density until it crosses back above `rho0`, at which point
   `P ~ (rho/rho0)^7` produces a wildly oversized restoring force. Fix:
   `TaitEOS::pressure()` clamps to `max(0, P)`. This is a standard,
   pragmatic mitigation in production weakly-compressible SPH codes for
   exactly this free-surface case (dam break *is* a free-surface
   problem); the cost is not modeling true tensile/cavitation stresses,
   which this project has no need to model.

2. **Floor-impact force spike.** Even with pressure clamped, the
   simulation still diverged, always around the same simulated time
   regardless of `c0`/viscosity tuning -- which was the tell that it
   wasn't a generic stiffness issue. Instrumenting `main.cpp` with
   `minY` / `maxSpeed` diagnostics pinned it exactly: the block's bottom
   face (a ~19x19 grid of particles) reaches the floor within the same
   few timesteps, and briefly compacts. `P/rho^2 ~ rho^(gamma-2)` means
   even a modest, transient density overshoot produces a very large
   force; with a *fixed* timestep (again, adaptive is V2 scope), one
   particle went from 0.6 m/s to 6,500 m/s to floating-point infinity in
   5 simulation steps. Fix: `PredictorCorrectorIntegrator` clamps speed
   to a configurable ceiling (`max_speed` in config, default 20 m/s) --
   far above any real dam-break flow velocity, so it only ever
   intervenes during genuine numerical pathology. It's a numerical
   safety valve, not a physical model, and is documented as such at the
   call site.

   Also changed: the spec's wall response is "flip velocity component"
   (a bounce). That's fine for a rigid body, but wrong for a fluid --
   real water doesn't ricochet off a floor -- and with ~361 particles
   hitting simultaneously, reflecting all of them back into the
   still-falling particles above was itself contributing to the
   compaction event. `wall_damping` (config) now defaults to a mostly
   *absorbing* 0.05 instead of a bouncy 0.5.

**Honest caveat:** even with both fixes, the velocity clamp engages
continuously (not just as a rare safety net) for a few hundred steps
after floor impact, before the flow spreads out and settles -- density
stays bounded (roughly 1.3-1.8x rest density at the impact zone, not
diverging further) and nothing goes NaN or leaves the domain, satisfying
the stability requirement, but it indicates the
floor-impact event is still under-resolved for this fixed timestep. The
principled fix is either adaptive timestepping (shrink `dt` when
velocities spike -- already scoped for V2) or boundary force particles
(a continuous repulsive field near walls, so particles are slowed before
they ever reach the boundary, rather than colliding with it). Both are
natural next steps.

## Visualization layer

`aquasph_view` (built only when `AQUASPH_BUILD_VISUALIZATION=ON`) renders the
live simulation rather than only benchmarking it. It is a separate executable
by design: the headless solver and the entire test suite have no reason to
link GLFW, and keeping that dependency isolated means the core project builds
and tests on machines without GL development headers -- including minimal CI
runners.

Both executables call the same physics functions in the same order
(`grid.build` -> `computeDensityPressure` -> `computeForces` ->
`integrator.step`) and share the same scenario initializer
(`core/DamBreakInit.*`, extracted from `main.cpp` precisely so the two entry
points cannot drift apart). What is rendered is provably the same simulation
that is benchmarked, not a simplified stand-in.

Module map (`src/render/`):

```
render/GLLoader.*          OpenGL 3.3 core function loader
render/Shader.*            GLSL compile/link + uniform helpers
render/Camera.*            Mouse-orbit camera (view + projection matrices)
render/ParticleRenderer.*  VAO/VBO particle upload + point-sprite draw
render_main.cpp            Window/context setup, input handling, render loop
```

### Why a hand-written GL loader rather than GLAD/GLEW

Modern OpenGL entry points -- essentially everything past the GL 1.1
fixed-function subset -- are not guaranteed to be link-time symbols in the
platform's GL library. The portable way to obtain them on every platform is a
runtime lookup, which GLFW exposes uniformly as `glfwGetProcAddress`. A loader
is therefore the correct tool rather than a workaround, and `GLLoader.hpp`
declares only the ~30 entry points this renderer actually calls instead of
pulling in a multi-thousand-line generated header for that surface area.

Every signature and enum value was cross-checked against the Khronos OpenGL
registry (`registry.khronos.org/OpenGL-Refpages`, `api/GL/glcorearb.h`) rather
than written from memory. That check caught a real error: `GL_PROGRAM_POINT_SIZE`
is a plausible-looking but incorrect symbol -- the actual constant is
`GL_VERTEX_PROGRAM_POINT_SIZE` (`0x8642`). No compiler warning would flag a
wrong-but-valid integer constant, and the visible symptom would have been
subtly incorrect point sizing rather than an obvious failure.

A consequence of this design worth noting: the built binary has no link-time
dependency on `libGL` or `libX11` at all (confirmed with `ldd`) -- GLFW
resolves those through `dlopen` at runtime. This is why the build requires no
`find_package(OpenGL)`.

### Rendering technique

Particles are drawn as `GL_POINTS`, colored by speed (deep blue at rest,
white at maximum velocity) and rounded into circles in the fragment shader via
a `gl_PointCoord` distance discard, rather than instanced spheres or
billboarded quads. The speed colormap makes the floor-impact event and
residual turbulence immediately legible against the settled fluid, which is
the diagnostic value the viewer exists to provide.

## Platform requirements for the viewer

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DAQUASPH_BUILD_VISUALIZATION=ON ..
make -j4
./aquasph_view                          # default scenario
./aquasph_view --particles 20000        # denser block
```

Controls: left-drag to orbit, scroll to zoom, `Esc` to quit.

**Linux/X11.** GLFW's X11 backend requires development headers at configure
time, not merely the runtime shared libraries. On Debian/Ubuntu:

```bash
sudo apt install libglfw3-dev mesa-common-dev libgl1-mesa-dev \
  libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libx11-dev
```

If `libglfw3-dev` is present, `find_package(glfw3)` uses it directly and the
`FetchContent` path is skipped entirely. A system with the runtime `.so` files
but no `-dev` packages will fail GLFW's own configure step with
`RandR headers not found`, which is the most common cause of a failed
visualization build on Linux.

**macOS.** No additional system packages beyond a standard Xcode toolchain,
but the OpenMP cache variables from the README are still required, since
`aquasph_view` links `aquasph_core`:

```bash
cmake -DCMAKE_BUILD_TYPE=Release -DAQUASPH_BUILD_VISUALIZATION=ON \
  -DOpenMP_CXX_FLAGS="-Xpreprocessor -fopenmp -I$(brew --prefix libomp)/include" \
  -DOpenMP_CXX_LIB_NAMES="omp" \
  -DOpenMP_omp_LIBRARY="$(brew --prefix libomp)/lib/libomp.dylib" \
  ..
make -j4
```

**Headless environments.** The viewer requires a real display and working
OpenGL 3.3+ drivers. GLFW does not silently fall back to a non-rendering
backend, so on a headless machine `glfwInit()` fails immediately with a clear
diagnostic rather than producing a black window -- `render_main.cpp` surfaces
that message directly along with a pointer to the package list above. This is
also why CI builds with `AQUASPH_BUILD_VISUALIZATION=OFF`.


## Build note: dependencies via FetchContent

`glm`, `nlohmann_json`, and `GoogleTest` are resolved via
`find_package(... QUIET)` first (so a machine with them installed via
apt/vcpkg/etc. just uses those), falling back to CMake `FetchContent`
(pulled from GitHub at configure time) if not found. No dependency
source is vendored into this repository.

## Known limitations and future work

- `LinkedCell::build` is still serial (see "Parallelization strategy" above for
  why, and what parallelizing it correctly would require).
- Fixed timestep. Adaptive `dt` (shrinking it when the CFL condition or
  a max-force/velocity check demands it) would remove the need for the
  velocity clamp described above and let higher-fidelity floor-impact
  behavior actually be resolved instead of clipped.
- Boundary handling is a simple clamp+damp, not boundary-force particles
  or ghost particles -- adequate for the solver but the standard place
  production SPH codes invest next.
- AoS particle layout (see above) -- benchmark data hints at a
  memory-bandwidth cost at scale, but that's unconfirmed without actual
  profiler output.
- Benchmark methodology couples particle count with local density (see
  `benchmarks/scaling_results.md`); a fixed-density scaling benchmark
  would better isolate the linked-cell search's O(N) behavior.
- No persistent OpenMP thread pool: each of the four parallel regions
  per step spawns/joins its own threads (the default `#pragma omp
  parallel for` behavior) rather than reusing one long-lived pool across
  the whole run. Likely a real, if modest, contributor to the
  sub-4x speedup at 4 threads -- worth measuring with `OMP_WAIT_POLICY`
  tuning or restructuring around one outer parallel region if profiling
  ever points at thread spawn/join as a bottleneck.
