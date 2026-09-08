# AquaSPH -- Architecture and Design Decisions

## Overview

A CPU-based 3D Smoothed Particle Hydrodynamics solver with the hot
per-particle loops parallelized via OpenMP, and a scenario laboratory
built on top of it. Each step it computes density, pressure, and forces
through kernel-weighted neighbour sums, integrates with an adaptive
predictor-corrector scheme, and reports machine-readable measurements.

**v1** was one hardcoded dam break with a flat global config. **v2** is a
composition engine: a scenario is a domain, materials, fluid volumes,
emitters, sinks, obstacles, forces and wave generators described in JSON,
and adding a new phenomenon means a new file rather than new solver code.
It also fixed two real bugs in the v1 force law that no amount of
formula-checking had found, because the simulation ran and looked
plausible with both of them in place. Those are sections 4 and 5 below.

This document is cumulative. The v1 sections are kept as written, because
the record of what was wrong and how it was found is the most useful thing
in here; v2 sections extend them rather than replacing them.

The physics was built and validated single-threaded before any
parallelization was introduced, and the parallelization was then verified
to leave results bit-identical. See "Determinism check" in
[`benchmarks/scaling_results.md`](../benchmarks/scaling_results.md) for
how that was measured, and `docs/experiments.md` for what the guarantee
does and does not cover.

## Module map

```
core/Particle.hpp          Particle struct (AoS layout), fluid or boundary
core/Constants.hpp         Shared physical/numerical constants
core/Material.hpp          Per-fluid physical parameters
core/SPHKernel.*           Cubic spline kernel W(r,h) and its gradient
core/DensityPressure.*     rho_i = sum m_j W_ij; Tait EOS pressure
core/BoundaryVolume.*      Akinci (2012) boundary-particle volumes
core/ForceCompute.*        Pressure, viscosity, surface tension, XSPH, boundary coupling
core/TimeStep.*            Adaptive dt from the CFL/force/viscous triple
core/ParallelReduce.hpp    Thread-count-independent reductions
core/Integrator.*          Predictor-corrector integration + domain faces
spatial/LinkedCell.*       Uniform-grid neighbour search, CSR storage
scene/Shapes.*             Box/sphere/cylinder/heightfield, sampling
scene/TimeSeries.*         One f(t) for schedules, forces and prescribed motion
scene/Scenario.*           The composition: domain, materials, regions, ...
scene/ScenarioLoader.*     JSON -> Scenario
scene/Simulation.*         Owns the particles; runs the step pipeline
scene/Quality.hpp          Resolution presets
metrics/Metrics.*          Machine-readable measurement and probes
render/*                   GL loader, shaders, camera, SSFR, environment, capture
benchmark/PerfTimer.hpp    Wall-clock timing utility
main.cpp                   Headless scenario runner
render_main.cpp            Viewer and offline frame recorder
```

`io/ConfigLoader.*`, `core/DamBreakInit.*` and `configs/default.json` were
**removed** in v2, not kept alongside. A bespoke initializer and a flat
global config for one scenario is how a scenario engine quietly becomes a
demo with a config file. Everything now goes through
`scene::Scenario` -> `scene::Simulation`, including the viewer, so what is
rendered is provably the simulation that is benchmarked and validated.

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

`LinkedCell::build` stayed serial in v1, and deliberately so: it wrote
into shared per-cell buckets (`std::vector<int>` per cell), and multiple
particles from different threads can legitimately land in the same cell,
so a naive parallel `push_back()` there **would** race.

**v2 restructured it to a counting sort over compressed-sparse-row
storage**, which is the change v1 said would be needed -- but the reason
turned out to be different from the one predicted, and the outcome is
better. Profiling said build() was 4.3% of step time, not the emerging
bottleneck this file expected; what it *did* say is that a bucket per
cell meant every neighbour query chased 27 independent heap pointers,
inside the loops that are ~95% of step time. See "Known limitations" at
the end for the measured before and after, and `spatial/LinkedCell.hpp`
for why the scatter pass is still deliberately serial and index-ordered
(a parallel scatter would reorder each cell's contents, and a cell's
order is the summation order of every neighbour loop that reads it).

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

v1's benchmark sweep gave a first, imperfect data point on this: parallel
efficiency at 4 threads declined from ~87% at 5,832 particles to ~76% at
50,653, the opposite of the usual "overhead amortizes better at scale"
pattern. The reading offered at the time was that AoS was costing memory
bandwidth at large N, which four threads sharing a fixed bus would show
as exactly that decline.

**v2's measurements weaken that hypothesis.** After the CSR neighbour
restructure, efficiency at 4 threads is 81% / 87% / 81% at low / medium /
high resolution -- no monotone decline, and the best figure is in the
middle of the range. Some of what v1 attributed to the particle layout
was the neighbour structure's pointer chasing instead. The AoS/SoA
question is therefore *more* open than v1 concluded, not less, and still
should not be answered without a cache-miss profiler.

v2 also grew the struct: `normal` and `xsphDelta` add 24 bytes to a
48-byte particle. Both are per-step scratch and could live in separate
parallel arrays; they are in the struct because both are read a step
later by code that already has the `Particle` in hand, so hoisting them
out means threading two more spans through every signature for a 24-byte
saving. If an SoA rewrite ever happens, that is where it starts.

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

### 4. The momentum equation mixed forces and accelerations

`computeForces` seeded its accumulator with `m_i * gravity` -- a force, in
newtons -- and then added the SPH pressure and viscous terms to it. Those
terms are *accelerations*:

    [m_j (P/rho^2) gradW] = kg * (Pa / (kg/m^3)^2) * (1/m^4)
                          = kg * (N m^4 / kg^2) * m^-4
                          = N/kg = m/s^2

The integrator then divided the whole accumulator by `m_i`, so gravity
came out correct at `g` and every SPH term came out at `1/m_i` times its
true value. At the shipped scenario's `m_i = 0.091 kg` that is an
**eleven-fold overstatement of every pressure gradient in the
simulation**.

Confirmed before changing anything, by driving the shipped code with two
particles at a known density and comparing its stored value against the
textbook acceleration and the textbook force: the stored value matched the
acceleration exactly and the force by a factor of `1/m_i`.

The fix is not a stray multiply. The accumulator is now an acceleration
for the entire loop and is converted to a force exactly once, at the
bottom -- which is what makes this class of error impossible rather than
merely fixed. `tests/test_forces.cpp::StoresForceNotAcceleration` pins it.

### 5. The viscosity term had the wrong sign, and was adding energy

The corrected viscous discretisation (bug 2 above) still had its two
factors paired the wrong way round:

    force += mu * m_j * (v_j - v_i) * (2 * dot(r_ij, gradW_ij) / denom) / rho_j

`dot(r_ij, gradW_ij)` is `|r_ij| * dW/dr`, and `dW/dr < 0` everywhere
inside the support, so that scalar is **negative everywhere**. Pairing it
with `(v_j - v_i)` yields a term along `+(v_i - v_j)`: the "viscosity"
accelerated particles *apart* in proportion to their relative velocity.
It was an anti-diffusion term injecting energy exactly where a viscous
term must remove it.

Measured directly on the shipped code before the fix: two particles in
pure shear, at exactly rest density so pressure was zero, each received
**+86 N along its own velocity**.

Morris, Fox & Zhu (1997) pair the negative scalar with `(v_i - v_j)`,
which damps. `tests/test_forces.cpp::ViscosityOpposesRelativeVelocity`
is the regression test.

These two bugs together are why the v1 velocity clamp engaged on every
step of every run rather than acting as the rare safety net it was
documented as. See "Phase 0" below for the measured before and after.

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

## Phase 0: numerical fidelity

v1 documented, honestly, that "the velocity clamp engages continuously
(not just as a rare safety net) for a few hundred steps after floor
impact". Running the shipped v1 code showed it was worse than that: on the
default 1,000-step dam break the reported maximum speed was **exactly
20.0000 m/s -- the clamp value -- at every single checkpoint from step 100
onward**, and the average SPH density fell from 953 to 447 kg/m^3. The
fluid was not settling; it was dispersing into a spray whose only
containment was an artificial ceiling on speed.

That is survivable for a dam break judged on "did it produce NaN". It
would destroy a droplet crown, a wave train, or any fluid-structure
interaction, and it becomes glaring the moment the surface is rendered
smoothly. So the cause was fixed before any rendering work.

### What changed, in order

1. **The momentum equation** (bugs 4 and 5 above): pressure gradients were
   11x too strong, and viscosity was adding energy instead of removing it.
2. **Adaptive timestep** (`core/TimeStep.*`): `dt = min(dt_cfl, dt_force,
   dt_viscous)` evaluated against the current state every step, with
   configurable coefficients, clamped bounds, and order-of-magnitude
   change logging.
3. **The velocity clamp was deleted.** Not replaced with a different
   ceiling -- deleted. `PredictorCorrectorIntegrator` no longer takes a
   maximum speed.
4. **XSPH** (Monaghan 1989) applied to advection only, never to the force.
5. **Surface tension** (Akinci et al. 2013): cohesion plus a curvature
   term from a colour-field normal, per material.
6. **Boundary particles** (Akinci et al. 2012) replacing clamp-and-damp
   walls.

### Measured, on the same geometry

`validation/v1_reference_setup.json` reproduces v1's exact configuration
-- 2x2x2 domain, 0.9^3 column, h = 0.1, c0 = 25, clamp-and-damp walls, no
boundary particles -- so the physics changes can be measured rather than
argued about. One second of simulated time, four threads:

| | v1 (as shipped) | v2, same geometry, no boundary particles | v2 `dam_break` scenario |
|---|---|---|---|
| particles | 8,000 | 9,261 | 21,525 fluid + 39,314 boundary |
| density min / avg / max | 271 / **447** / 1334 | 232 / **851** / 1303 | see `results/` |
| within 1% of rest density | not measured | 8.1% final | ~80% |
| max speed | **20.00 (pinned at the clamp)** | 15.3 m/s | physical |
| clamp activations | continuous, every step | **clamp removed** | **clamp removed** |
| timestep | fixed 1.0e-3 s | adaptive, 6.2e-4 - 1.0e-3 s | adaptive |

The middle column isolates the *physics* fixes: with the same crude
clamp-and-damp walls, average density recovers from 447 to 851 kg/m^3 and
the clamp is gone entirely. The remaining deficit is the wall model, which
is what boundary particles fix -- the right-hand column, and the
hydrostatic test below.

### Hydrostatic equilibrium: the boundary particles' own test

The cheapest complete statement that the boundary coupling, the pressure
law and the timestep controller all agree is that **still water stays
still**. A 0.3 m closed tank filled to 0.2 m, released from rest, run for
1 s (6,174 fluid + 5,482 boundary particles):

| quantity | value |
|---|---|
| density min / avg / max | 354 / **1002** / 1089 kg/m^3 |
| within 1% of rest density | **96.1% final, 92.7% mean** |
| max speed after settling | **0.03 m/s** |
| fluid volume, start / end | 0.0210 / 0.0208 m^3 |
| containment activations | 579, over 2,742 steps x 6,174 particles |

The low minimum density is free-surface particles, which genuinely have an
incomplete kernel neighbourhood; that is a property of SPH, not a defect.

### The containment counter had to be fixed before it meant anything

The first version of that table read **4,049,728 containment events**.
None of them were penetration. With boundary particles enabled, fluid at
rest settles with its last layer sitting essentially *on* the nominal
domain plane -- the true no-penetration surface is about half a particle
spacing further out, midway to the first boundary layer -- so at zero
tolerance every one of those particles tripped containment on the negative
half of its sub-micrometre jitter, on every step. Worse, each trip zeroed
a velocity component, so the counter was not merely uninformative: it was
applying a small artificial damping along every wall.

A quarter-spacing tolerance removed both. 4.0M events became 579, and the
number now measures what it claims to.

### Cost of the additions

Per-stage timings are in `benchmarks/scaling_results.md`. XSPH is
accumulated inside the existing force neighbour loop, so it costs one
extra kernel evaluation per pair and no extra traversal. Surface tension
adds a separate normals pass, but that pass **short-circuits entirely when
no material in the scenario has a non-zero surface-tension coefficient** --
so the dam break, the flood and the wave tank pay nothing for it at all,
which is measurable in the profile as `surface normals: 0.000 ms/step`.

## How well the boundary is resolved

The Akinci volume weighting `V_b = 1 / sum_k W(r_bk)` is what makes a
wall's strength independent of how finely it happens to be sampled. It
delivers that only once enough boundary layers fall inside the kernel
support, and that count is roughly `h/spacing - 1`. At this project's
`h/spacing = 2`, a fluid particle resting on a wall sees exactly **one**
layer.

Measured against the continuum limit -- the density a solid half-space
should contribute, computed by direct quadrature of the kernel below a
plane (`tests/test_boundary.cpp::halfSpaceLimit`), for a probe 0.01 m
above the surface with h = 0.1:

| h / boundary spacing | boundary sum | fraction of the half-space limit |
|---:|---:|---:|
| 2 | 80.5 | **~28%** |
| 4 | 290.4 | ~83% |
| 8 | 410.3 | ~112% |

So the operating point is genuinely under-resolved, and it converges from
below. This is recorded rather than hidden because it is a real property
of the discretisation, and
`ContributionConvergesTowardTheHalfSpaceLimitFromBelow` asserts both the
convergence *and* the shortfall so a future change cannot quietly make it
worse.

**Why it is still the default.** `boundary_spacing_scale` exists to sample
the wall more finely. It was implemented and measured, on the hydrostatic
tank:

| | scale 1.0 | scale 0.5 |
|---|---|---|
| boundary particles | 5,482 | 42,004 |
| fluid remaining after 1 s | 6,174 | **3,116** |
| max speed | 2.35 m/s | **11.8 m/s** |
| max density | 1089 | **1417** |
| ms/step | 8.0 | 10.1 |

Halving the boundary spacing puts the first boundary layer half a fluid
spacing from the resting fluid, where the kernel is near its peak. The
near-wall layer over-pressurises and ejects half the fluid out of the
open top of the tank. The finer sampling is available, costed, and off by
default, with the measurement here rather than a guess in a comment.

What the coarse boundary *does* achieve is measured too: hydrostatic
equilibrium holds to 96% within 1% of rest density, and
`FluidDoesNotLeakThroughTheFloorUnderImpact` drops a column onto a floor
and asserts that not one particle ends up on the far side of it. The
density deficit is compensated in practice by the mirrored pressure term,
which acts whether or not the density sum is complete.

## Dynamic particle counts

Emitters make the particle array grow during a run and sinks shrink it.
That is the single change most likely to break the bit-identical guarantee,
because it introduces *order* as a variable. Each consequence was audited
rather than assumed:

| concern | resolution |
|---|---|
| `LinkedCell` capacity | Its scratch arrays grow with the particle count and never shrink; `build()` reads `particles.size()` each call. No per-step allocation once grown. |
| `LinkedCell` rebuild | Rebuilt from scratch every step regardless, so a changed count needs no special case. |
| OpenMP loop bounds | Every loop reads `particles.size()` at entry. The array is never mutated *during* a parallel region. |
| Per-thread neighbour buffers | Allocated per thread inside the parallel region, reused across that thread's iterations, independent of the particle count. |
| Memory strategy | `Simulation` reserves an estimate of initial fluid + emitter output up front, so the array does not reallocate mid-run. This is an optimisation, not a correctness requirement -- nothing holds a `Particle*` across a step -- but reallocation would show as periodic step-time spikes. |
| Unbounded growth | `--max-particles` (default 4,000,000, boundary particles included) stops emission with a warning rather than exhausting memory. |
| Rendering buffers | The GPU buffers grow with headroom (2x) rather than exactly, so a scenario adding a layer every few steps does not stall the pipeline every few steps. |
| Statistics | Every aggregate is a deterministic fixed-chunk reduction over the current fluid range. |
| **Determinism** | **Insertion is in (emitter index, lattice site index) order; removal is a stable, serial compaction that preserves relative order. Boundary particles are kept at the FRONT of the array so obstacle motion can address them by fixed index while the fluid population changes behind them.** |

`tests/test_dynamic_particles.cpp` asserts bit-identical output across
1/2/4/8 threads with emitters and sinks both active, and that boundary
particles stay at the front.

## Scenario architecture

A scenario is a **composition of primitives**, never a subclass. The test
the architecture has to pass is that adding a new phenomenon means new
JSON plus existing primitives, not new solver code. The fourteen scenarios
in `configs/scenarios/` contain no C++ between them.

Two design decisions are worth stating because the obvious alternative
looked reasonable:

**One `TimeSeries` instead of a force hierarchy.** The brief asked for
`ConstantForce`, `SinusoidalForce`, `PulseForce` and `ScriptedForce` as
separate classes. They are the same function shape applied along different
vectors, so they are one struct plus a direction -- and the same struct
then also drives emitter schedules and prescribed boundary motion. An
interface with four implementations that differ only in a closed-form
expression is not an abstraction; it is four copies of a switch.

**One tagged-union `Shape` instead of five subclasses.** Every consumer --
fluid regions, obstacles, emitters, sinks, metric probes -- asks the same
three questions (contains, contains-after-erosion, bounds), all five
answers are a handful of arithmetic, and the set is closed by the schema.
Virtual dispatch would buy nothing and would put an allocation and an
indirect call inside the sampling loops. Erosion is what turns a solid
into a boundary shell, which is why it is a first-class operation.

**A wave generator IS an obstacle** with prescribed motion -- a piston
paddle. That was chosen over imposing a free-surface displacement
`eta(x,t)` directly, because a prescribed surface is a rendering trick the
fluid does not actually obey, whereas a paddle makes waves the solver has
to propagate itself. Amplitude, period and celerity therefore become
measurements rather than inputs, which is the whole difference between a
wave tank and a wave effect.

## Bugs found by measuring, in v2

Two more, both found by running the thing rather than reading it, and both
of the kind that produce a plausible result rather than a crash.

**Emitters with an aperture thinner than one particle spacing emitted
nothing, silently.** `container_fill` ran to completion, reported STABLE,
and produced zero fluid. Its emitter is a 12 mm-thick disc; at a 16.2 mm
spacing the single candidate lattice plane landed one float ULP outside
the cylinder's own half-thickness test. Emitters now sample on a
centre-anchored lattice, which guarantees the centre plane exists however
thin the aperture is, and say so loudly on stderr if they still find no
sites.

**The stage profiler double-counted.** The integrator re-evaluates forces
through a callback, so the stage timers nested: the mid-step force
evaluation was charged to both `forces` and `integration`. That reported
integration as 27% of step time and summed the columns to 260 ms/step
against a measured 189 ms/step wall clock. Nested time is now subtracted
so every column is exclusive. Worth recording because the wrong profile
would have sent optimisation effort straight at the integrator, which
turned out to be 1% of step time.

## Known physical approximations

Everything below is a deliberate limitation of what this solver models. It
is collected in one place so that no result has to be read in the hope
that its caveats were remembered.

- **Weak compressibility.** The Tait EOS admits ~1% density fluctuation by
  design. This is not incompressible flow; it is compressible flow with an
  artificially low sound speed, chosen so the CFL step stays affordable.
  Acoustic waves are physically wrong by construction.
- **Pressure is clamped non-negative.** True tensile stress and cavitation
  are not modelled. This is the standard mitigation for the free-surface
  tensile instability (see above) and is why the free surface stays
  coherent at all.
- **Finite resolution.** Every scenario runs at a particle spacing of
  centimetres to millimetres. Anything smaller -- droplet break-up,
  boundary layers, thin films, the turbulent cascade -- is unresolved and
  is represented, if at all, by artificial viscosity.
- **Artificial viscosity.** `Material::viscosity` is 10^3 to 10^4 times
  water's physical 1e-3 Pa*s. It is a sub-particle dissipation model.
  Reynolds numbers must not be read off it.
- **Boundary approximation.** Akinci boundary particles at
  `h/spacing = 2` recover ~28% of a solid half-space's density
  contribution (measured above). Walls hold water and do not leak, but the
  near-wall density field is not converged.
- **No-slip is a coefficient, not a boundary layer.**
  `boundary_friction` scales the viscous term against walls. The velocity
  profile within one particle spacing of a wall is not resolved, so wall
  shear stress is not predictive.
- **Surface tension is a particle-level model** (Akinci cohesion +
  curvature), not a resolved interface. It reproduces the right qualitative
  behaviour -- droplets round up, crowns form -- at a scale where the true
  interface thickness is far below the particle spacing.
- **Unresolved turbulence.** No LES, no sub-grid stress model. Wakes are
  laminar at these resolutions.
- **No true multiphase physics.** Multiple materials can coexist and each
  particle carries its own, but there is no interface tension between
  different materials, no density-ratio-stable pressure formulation, and
  no mixing or phase change. Air is not modelled at all, which is why a
  plunge pool does not turn white.
- **Terrain is analytic**, not survey data: a plane plus Gaussian bumps.
  Real topography cannot be imported.
- **Wave generation is a linear piston.** Linear (Biesel) theory is quoted
  only below H/L ~ 1/20 and is flagged when the measured steepness exceeds
  it. Stokes waves, shallow-water theory and irregular spectra are
  extension points, not implemented.
- **Moving boundaries are kinematic.** Prescribed translation only: no
  rotation, and no rigid-body dynamics. The fluid exerts no force back on
  a solid.
- **Tier 2 scale limits.** The flood, flash flood, coastal wave, waterfall,
  fountain and tsunami-pulse scenarios are metre-scale flume experiments
  rendered at metre scale. They are not scaled models of kilometre-scale
  events: Froude similarity is not enforced, Coriolis and long-wave
  dispersion over ocean depths are absent, and nothing about them
  transfers to a real site.

## Extension points (Tier 3): documented, not implemented

The architecture leaves room for these. **None of them is implemented, and
no claim is made about any of them.** An honest extension point is worth
more than a shallow implementation, and each entry below says where it
would attach and what it would actually cost.

| extension | where it attaches | the hard part |
|---|---|---|
| **Erosion / sediment transport** | A per-particle scalar concentration field alongside `density`, advected in the existing neighbour loop; a shear-stress threshold at boundary contact | Coupling concentration back to density and viscosity, and a bed-elevation model that changes the heightfield during the run -- which means recomputing boundary volumes |
| **Mudflow / non-Newtonian rheology** | `Material` gains a yield stress; `ForceCompute`'s viscous term takes a strain-rate-dependent `mu` | The apparent viscosity depends on the strain-rate tensor, which needs a second neighbour pass to compute |
| **Multiphase flow** | Already partly present: per-particle material index | A density-ratio-stable pressure formulation and an interface tension model. The current single-phase discretisation is dominated by artefacts at large density ratios |
| **Foam / spray / air entrainment** | A diagnostic pass after `computeDensityPressure` flagging particles by trapped-air potential; a separate render pass | Deciding what foam *is* dynamically, not just visually. As a pure render effect it would be dishonest, which is why it is absent |
| **Rain** | An emitter shape spanning the domain roof with a stochastic schedule | Determinism: the RNG stream would have to be a pure function of scenario and step count |
| **River networks** | Several emitters and sinks plus terrain, expressible in JSON today | Sustaining steady flow over a long channel needs far more particles than these resolutions allow |
| **Moving rigid bodies, gates, pistons** | `Motion` already prescribes translation; gates and pistons work now | *Dynamics* -- integrating a solid's momentum under fluid force -- needs force accumulation on boundary particles and a rigid-body integrator |
| **Rotating boundaries** | An `orientationAt(t)` beside `translationAt(t)` in `Motion` | Boundary particles must be re-transformed, not merely offset. Akinci volumes survive rigid motion unchanged, so this is the cheapest of the Tier 3 items |
| **Porous boundaries** | A per-obstacle permeability scaling the boundary pressure term | A physically defensible model rather than "a wall that leaks on purpose" |
| **Ocean wave spectra** | `WaveGenerator` already superposes components | A JONSWAP/Pierson-Moskowitz spectrum needs a flume long enough for the components to separate, which is a resolution problem, not an interface one |
| **Mesh terrain** | A fourth `ShapeType` with `contains`/`containsEroded` | A robust point-in-mesh test and a watertightness story |
| **Periodic domains** | A face mode, superficially | This one was *implemented and then removed*. Wrapping positions is four lines and produces a spurious free surface at the wrap plane, because separations are computed directly: a particle at one end has no neighbours at the other. A real periodic domain needs the minimum-image convention in `LinkedCell::getNeighbors` **and** in every separation computed by `DensityPressure`, `ForceCompute`, `BoundaryVolume` and the normals pass -- a change to the solver, not a face mode |
| **CUDA / GPU solver** | The neighbour loops are already index-parallel with no cross-writes | The determinism guarantee. Bit-identical output across thread counts is a stated property, and reproducing it on a GPU means fixed-order reductions there too |

The test that this list is honest: each entry names a real attachment
point in code that exists, and a real reason it is not a weekend's work.

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

Two modes, both first-class.

**Surface** is screen-space fluid rendering after van der Laan, Green &
Sainz (I3D 2009): sphere-impostor depth, bilateral depth smoothing,
normals from the smoothed depth, additive thickness, and a Fresnel /
refraction / Beer-Lambert composite. No mesh extraction, no marching
cubes. See [`docs/rendering.md`](rendering.md).

**Points** draws speed-coloured sprites, as v1 did. It is kept not for
compatibility but because it is the diagnostic view: when a reconstructed
surface looks wrong, the first question is whether the particle
distribution under it is wrong, and only this mode answers that. It is
also the performance baseline the surface is measured against.

One v1 rendering bug worth recording, since it is exactly the class of
error this project documents elsewhere: the speed colormap divided by
`cfg.maxSpeed` while the integrator clamped speed to *exactly*
`cfg.maxSpeed`. During the interesting phase of every run, nearly every
particle therefore mapped to 1.0 and the fluid rendered as a flat white
sheet -- a visualisation whose normalisation was tied to a physics limit,
showing nothing. Colour now normalises against the scenario's
`render.reference_speed`, which the solver never reads.

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

Items struck through were open in v1 and are now closed; they are kept so
the record reads as a history rather than a snapshot.

- ~~`LinkedCell::build` is still serial.~~ **Resolved, but not the way
  this file predicted.** v1 argued that build() would become the
  bottleneck once the O(N*neighbours) loops were threaded, and that a
  counting-sort restructure would then be worth it. Profiling at 218k
  particles says build() is **4.3%** of step time at four threads, so
  parallelising it caps out at a ~3% total gain -- the prediction was
  directionally right and quantitatively not worth acting on for its own
  sake. What the profile *did* show is that `vector<vector<int>>` made
  every neighbour query dereference 27 independent heap pointers inside
  the density and force loops, which are ~95% of step time. `LinkedCell`
  is therefore a compressed-sparse-row structure now, built by counting
  sort with nothing allocated per step, which sped up build (8.2 -> 3.7
  ms), density (36.9 -> 32.9 ms) and forces (140.4 -> 134.5 ms) together.
  Its scatter pass stays deliberately serial and index-ordered: the
  textbook parallel version claims slots with atomic fetch-add, which
  would leave each cell in thread-scheduling order, and a cell's order is
  the summation order of every neighbour loop that reads it.
- ~~Fixed timestep.~~ **Resolved.** `core/TimeStep.*`, and the velocity
  clamp it existed to justify is gone.
- ~~Boundary handling is a simple clamp+damp.~~ **Resolved.** Akinci
  (2012) boundary particles, sharing the fluid array so the existing
  neighbour search finds them. See "How well the boundary is resolved"
  for what that does and does not achieve, measured.
- **A persistent OpenMP thread pool** is still absent; each parallel
  region spawns and joins. Measured speedup at four threads is 3.2-3.5x
  across the resolution range, so the ceiling this imposes is real but
  modest.
- **SoA particle layout** remains unexplored, and the v1 hypothesis that
  AoS cache pressure explains the efficiency decline at scale is now
  *less* supported: after the CSR change, parallel efficiency at four
  threads is 81% / 87% / 81% at low / medium / high resolution rather than
  declining monotonically. Still unconfirmed without a profiler.
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
