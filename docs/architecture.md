# AquaSPH -- Phase 0 + Phase 1 Architecture

## What this is

A CPU, 3D Smoothed Particle Hydrodynamics (SPH) fluid solver, with the
hot per-particle loops parallelized via OpenMP (Phase 1). Given a
dam-break initial condition (a block of particles in a box, under
gravity), it computes density, pressure, and forces every timestep via
kernel-weighted neighbor sums, integrates with a predictor-corrector
scheme, and reports FPS / stability. Phase 0 built and validated the
physics single-threaded; Phase 1 (this section) parallelized the four
loops that dominate per-step cost without changing any physics --
see "Determinism check" in
[`benchmarks/scaling_results.md`](../benchmarks/scaling_results.md) for
how that claim was actually verified, not just assumed.

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

## Phase 1: what got parallelized, and what didn't

Four loops dominate per-step cost: the neighbor-sum loop inside
`computeDensityPressure`, the neighbor-sum loop inside `computeForces`,
and the three particle-local loops inside
`PredictorCorrectorIntegrator::step`. All four are now threaded with
`#pragma omp parallel for schedule(static)` (or, for the two
neighbor-search loops, `#pragma omp parallel` with a per-thread
`neighbors` buffer wrapping the `#pragma omp for`, since each thread
needs its own scratch vector to call `LinkedCell::getNeighbors` into --
Phase 0's code had one shared `neighbors` vector reused every iteration
via `.clear()`, which is only safe single-threaded).

Each of the four loops is safe to parallelize over particles because
every iteration `i` writes only `particles[i]`'s own field(s) --
density+pressure in one loop, force in another, velocity/position in
the integrator's -- and only *reads* (never mutates) neighboring
particles' data. No two iterations ever write the same memory, so
there's nothing to race on and no locking/atomics needed. This is the
textbook "embarrassingly parallel" case, which is exactly why these
loops (and not, say, a hypothetical global reduction) were the Phase 1
target.

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
the simpler layout, and the deliberate Phase 0 choice: "no premature
optimization -- get it working first, parallelize in Phase 1." SoA
(separate `vector<vec3>` for position, separate `vector<float>` for
density, etc.) is generally more cache- and SIMD-friendly for the
density/force loops, since a loop touching only position and density
doesn't have to pull mass/pressure/force into cache alongside it. The
cost is every "particle" access becomes several parallel-array index
operations instead of one struct dereference -- more invasive, harder to
read, and premature before profiling shows AoS is actually the
bottleneck (vs. the O(N) neighbor search itself, or lock contention once
OpenMP is added in Phase 1).

Phase 1's benchmark sweep gives a first, imperfect data point on this:
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

## Three bugs found in the original spec, and why the fixes matter

This section exists because "why the cubic spline kernel," "why
predictor-corrector," etc. are explicit interview talking points for
this project. Repeating the spec's original formulas confidently would
have been wrong in ways worth understanding, not just worth fixing.

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
`dt` is fixed in Phase 0 (adaptive stepping is explicitly V2 scope),
the only lever is `c0`: solving `0.4*h/c0 = dt` for the *largest*
CFL-safe `c0` at `dt=0.001, h=0.1` gives `c0 <~ 40`. The shipped default
(`c0=25`) sits comfortably under that ceiling, trading a softer,
more-compressible EOS for a fixed, spec-mandated timestep.

## Two more issues found only by actually running the dam-break scenario

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
the literal Phase 0 stability requirement, but it's a sign the
floor-impact event is still under-resolved for this fixed timestep. The
principled fix is either adaptive timestepping (shrink `dt` when
velocities spike -- already scoped for V2) or boundary force particles
(a continuous repulsive field near walls, so particles are slowed before
they ever reach the boundary, rather than colliding with it). Both are
natural Phase 1 candidates.

## Phase 1.5: OpenGL/GLFW viewer

A second executable, `aquasph_view` (built only when
`AQUASPH_BUILD_VISUALIZATION=ON`), renders the live simulation instead of
just benchmarking it. Deliberately a *second* executable rather than
adding rendering to `aquasph`: the headless benchmark/CI binary has no
reason to link GLFW at all, and keeping it that way means a machine
without GL dev headers (a minimal CI runner, or this project's own dev
sandbox -- see below) can still build and test everything else with the
default `AQUASPH_BUILD_VISUALIZATION=OFF`. Both executables call the
exact same physics functions in the exact same order (`grid.build` ->
`computeDensityPressure` -> `computeForces` -> `integrator.step`) and
the exact same dam-break initializer (`core/DamBreakInit.*`, factored out
of `main.cpp` in this phase specifically so both entry points share it
instead of risking two copies drifting apart) -- what's on screen is
provably the same simulation `aquasph` benchmarks, not a simplified
stand-in.

Module map (`src/render/`):

```
render/GLLoader.*          Hand-rolled OpenGL 3.3 core function loader
render/Shader.*             GLSL compile/link + uniform-setting helpers
render/Camera.*              Mouse-orbit camera (view + projection matrices)
render/ParticleRenderer.*    VAO/VBO particle upload + point-sprite draw
render_main.cpp               Window/context setup, input, render loop
```

**Why a hand-rolled GL loader instead of GLAD/GLEW.** Modern OpenGL
functions (essentially anything past the GL 1.1 fixed-function subset)
aren't necessarily link-time symbols in the platform's GL library --
the portable way to obtain them, on every platform, is a runtime lookup
via `glfwGetProcAddress`. A loader is therefore the *correct* tool here,
not a workaround; `GLLoader.hpp` declares the ~30 entry points this
project's renderer actually calls rather than pulling in a
several-thousand-line generated header for that small a surface. Every
signature and enum value in it was cross-checked against the Khronos
OpenGL registry while writing it (`registry.khronos.org/OpenGL-Refpages`,
`KhronosGroup/OpenGL-Registry api/GL/glcorearb.h`) rather than typed from
memory -- one real near-miss doing that check: `GL_PROGRAM_POINT_SIZE`
(an easy, plausible-looking guess) turned out to not be the correct
symbol at all; the real one is `GL_VERTEX_PROGRAM_POINT_SIZE` (`0x8642`).
That's exactly the kind of silently-wrong-forever constant that no
compiler warning would ever catch, and which this project could not have
caught by just running the renderer and looking at it either (see below)
-- checking against the authoritative source was the only real defense.

**Rendering technique.** Particles draw as `GL_POINTS`, colored by speed
(still water -> deep blue, fast-moving particles -- e.g. the
floor-impact spike documented above -- -> white) and rounded into
circles in the fragment shader via a `gl_PointCoord` discard, rather
than instanced spheres or billboarded quads. Simpler, and deliberately
so: this renderer could not be visually test-run by its own author (see
below), so the simplest technique that's still visually informative was
preferred over a fancier one that would multiply the surface area for
an undetected bug.

## Investigating OpenGL/GLFW feasibility in this project's dev sandbox

This project was largely built in a sandboxed Linux VM with no display
server and no `sudo`. Before writing the renderer, that sandbox's actual
capability was checked empirically rather than assumed -- the findings
below are measured, not guessed, and shaped several of the decisions
above:

1. **Runtime GL/X11 libraries were present, dev headers were not.**
   `libGL.so.1`, `libX11.so.6`, `libXrandr.so.2`, `libXinerama.so.1`,
   `libXcursor.so.1`, and `libXi.so.6` were all installed, but
   `GL/gl.h`, `GL/glx.h`, and the `X11/extensions/Xrandr.h` (etc.)
   *headers* were not -- confirmed with `find /usr/include`, not assumed
   from the runtime libraries' presence. `apt-get install` (any of the
   `*-dev` packages that ship those headers) failed with a `403
   Forbidden` from the sandbox's package-mirror proxy, and there was no
   `sudo` to install them even if the mirror worked. This is why GLFW's
   own CMake configure fails on `GLFW_BUILD_X11=ON` in this specific
   sandbox with `RandR headers not found` -- confirmed directly by
   actually running that configure and reading the error, not inferred.

2. **EGL and OSMesa are entirely absent**, not just missing dev headers
   -- no `libEGL.so`/`libOSMesa.so` at all. That rules out the two usual
   "headless OpenGL" fallbacks.

3. **GLFW's null platform builds and links fine**, but does not itself
   provide a real GL context. A minimal standalone test
   (`glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_NULL)` +
   `glfwCreateWindow` requesting a GL 3.3 core context) failed with
   `OSMesa: Library not found` -- the null platform's own window backend
   tries to hand context creation off to OSMesa, which (per point 2)
   isn't there either.

4. **The real, unmodified `render_main.cpp` fails even earlier and more
   plainly.** Without forcing the null platform (which a normal
   `render_main.cpp` for a real machine correctly never does),
   `glfwInit()` itself returns false with GLFW's own message: *"This
   binary only supports the Null platform."* GLFW deliberately does not
   auto-select the null platform for `GLFW_ANY_PLATFORM` -- so a real
   application gets one clear, catchable, honest error instead of
   silently limping along on a backend that can't render anything.
   `render_main.cpp`'s `glfwInit()` failure branch prints this message
   verbatim along with pointers to the package list below.

**Conclusion: no OpenGL context is creatable in this sandbox, under any
GLFW backend, without root.** This is a genuine environment limitation,
not a code gap -- confirmed by exhausting every fallback GLFW itself
supports (X11, EGL, OSMesa, null), not by giving up after the first one.

**What was still verified, given that constraint.** The renderer's
CMake target (`aquasph_view`) was configured and built end-to-end against
a real, locally-built GLFW 3.4 (source from GitHub, configured with
`-DGLFW_BUILD_X11=OFF -DGLFW_BUILD_WAYLAND=OFF` so it would configure at
all in this sandbox) -- every file in `src/render/` plus
`render_main.cpp` compiled and linked with **zero warnings or errors**
under this project's `-Wall -Wextra`. The full existing test suite (20/20
GoogleTest cases) and the headless `aquasph` binary were re-verified
immediately after, to confirm the `DamBreakInit` refactor and other
Phase 1.5 changes caused no regression to the already-verified Phase 0/1
physics. Running the resulting `aquasph_view` reaches exactly the
`glfwInit()` failure described in point 4 above -- not a crash, not a
hang, a clean, informative, expected failure -- which is itself a form of
verification: the renderer's own code (config load, particle init, GLFW
setup sequence, error handling) runs correctly right up to the true
environment limit. `ldd` on the built `aquasph_view` confirms it has no
dynamic link-time dependency on `libGL`/`libX11` at all (GLFW resolves
those via `dlopen` at its own runtime, not at link time) -- a concrete
confirmation that this project's "no `find_package(OpenGL)`, resolve
everything through `glfwGetProcAddress`" design (see `GLLoader.hpp`)
is real, not just asserted.

**What was not, and could not be, verified here:** an actual rendered
frame. That needs a real display (X11, Wayland, macOS, or Windows) with
working OpenGL 3.3+ drivers -- any normal desktop/laptop satisfies this;
this project's own dev sandbox specifically does not.

**Update: since confirmed working on real hardware.** Built and run on
macOS (Apple Silicon, AppleClang 21, CMake 4.4.2) -- `aquasph_view` opens
a window, renders the dam-break block as speed-colored point sprites
against the dark clear color, updates live frame to frame, and responds
to mouse-orbit/scroll-zoom/Esc as designed. This closes out the one gap
everything above is explicit about: the renderer was correct code on
paper (compiled, linked, ran up to the display-creation call) before
this, and is now confirmed correct in practice, on the actual target
platform this phase was written for.

## Building the viewer on a real machine

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DAQUASPH_BUILD_VISUALIZATION=ON ..
make -j4
./src/aquasph_view                      # default scenario
./src/aquasph_view --particles 20000    # denser block
```

**macOS:** add the same OpenMP cache variables the headless build needs
(see "Build note" in README.md) -- `aquasph_view` links `aquasph_core`,
which requires OpenMP same as `aquasph` does:

```bash
cmake -DCMAKE_BUILD_TYPE=Release -DAQUASPH_BUILD_VISUALIZATION=ON \
  -DOpenMP_CXX_FLAGS="-Xpreprocessor -fopenmp -I$(brew --prefix libomp)/include" \
  -DOpenMP_CXX_LIB_NAMES="omp" \
  -DOpenMP_omp_LIBRARY="$(brew --prefix libomp)/lib/libomp.dylib" \
  ..
make -j4
```

Controls: left-click-drag to orbit, scroll to zoom, Esc to quit.

On Debian/Ubuntu, GLFW's X11 backend needs these dev packages (this is
the exact list this project's own sandbox was missing -- see above):

```bash
sudo apt install libglfw3-dev mesa-common-dev libgl1-mesa-dev \
  libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libx11-dev
```

If `libglfw3-dev` is already installed, CMake's `find_package(glfw3)`
picks it up directly and skips the `FetchContent` build entirely. macOS
and Windows need no extra system packages beyond a normal Xcode/MSVC
toolchain -- GLFW's CMake handles both natively.

## Build note: dependencies via FetchContent

`glm`, `nlohmann_json`, and `GoogleTest` are resolved via
`find_package(... QUIET)` first (so a machine with them installed via
apt/vcpkg/etc. just uses those), falling back to CMake `FetchContent`
(pulled from GitHub at configure time) if not found. No dependency
source is vendored into this repository.

## Known limitations / next candidates

- `LinkedCell::build` is still serial (see "Phase 1" section above for
  why, and what parallelizing it correctly would require).
- Fixed timestep. Adaptive `dt` (shrinking it when the CFL condition or
  a max-force/velocity check demands it) would remove the need for the
  velocity clamp described above and let higher-fidelity floor-impact
  behavior actually be resolved instead of clipped.
- Boundary handling is a simple clamp+damp, not boundary-force particles
  or ghost particles -- adequate for Phase 0/1 but the standard place
  production SPH codes invest next.
- AoS particle layout (see above) -- Phase 1's benchmark data hints at a
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
