# AquaSPH -- Benchmark Results

All numbers below are **measured, not estimated** -- captured from real
runs of `scripts/benchmark.sh` in Release mode, same build, same machine,
same session, immediately after a clean rebuild. Reproduce with:

```bash
scripts/benchmark.sh 40 dam_break
```

**Machine:** 4 physical cores (`nproc` = 4), GCC 13.3, `-O3`, Linux.
Timings are the `--profile` total, which is the sum of the exclusive
per-stage times and excludes process start-up and metrics output.

---

## Throughput and scaling (v2, `dam_break`)

Particle counts follow from the scenario's geometry and the quality
preset; `--quality` multiplies both `h` and the particle spacing by
1.8 / 1.0 / 0.55, so the neighbour count per particle is identical across
rows and only the sampling density changes.

| Quality | Fluid | Boundary | Total | 1 thread | 2 threads | 4 threads | 8 threads | Speedup (4T) | Efficiency |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `low`    | 3,864   | 13,176 | 17,040  | 18.62 ms | 9.70 ms  | **5.77 ms**  | 10.27 ms | 3.23x | 81% |
| `medium` | 21,525  | 39,314 | 60,839  | 107.68 ms | 55.32 ms | **30.88 ms** | 35.10 ms | 3.49x | 87% |
| `high`   | 118,844 | 98,892 | 217,736 | 587.74 ms | 313.36 ms | **181.76 ms** | 304.90 ms | 3.23x | 81% |

In steps per second: 173 / 32 / 5.5 at four threads.

**Boundary particles are the majority of the count at low resolution and
about 45% at high.** That is inherent to a thin tank: boundary count
scales with surface area over spacing squared, fluid count with volume
over spacing cubed, so the ratio improves as resolution rises. They are
cheap per particle -- they are skipped as loop subjects entirely, and the
loops start at the first fluid index rather than walking past them -- but
they are not free: they enlarge near-wall neighbour lists and the grid.

**8 threads on 4 physical cores is oversubscription, not parallelism**, and
the data shows it: worse than 4T at every resolution. This is expected,
textbook behaviour, reported rather than omitted. On real 8-core hardware,
expect the 8-thread column to look like the 4-thread column does here.

### Efficiency no longer declines monotonically with N

v1 measured 87% -> 76% efficiency as particle count grew, and attributed
it to AoS cache pressure. v2 measures 81% / 87% / 81%. The middle of the
range is now the best, not the worst. Some of what v1 attributed to the
particle layout was the neighbour structure's pointer chasing, removed by
the CSR restructure below. The AoS-vs-SoA question is therefore more open
than v1 concluded -- and still should not be settled without an actual
cache-miss profiler.

---

## Where the time goes: per-stage profile

`--profile` reports **exclusive** wall time per pipeline stage.
`dam_break --quality high`, 217,736 particles (118,844 fluid + 98,892
boundary), 40 steps:

| stage | 1 thread | share | 4 threads | share |
|---|---:|---:|---:|---:|
| linked-cell build | 6.60 ms | 1.1% | 3.70 ms | 2.1% |
| boundary volumes | 0.00 ms | 0.0% | 0.00 ms | 0.0% |
| density + pressure | 121.78 ms | 20.8% | 32.93 ms | 18.7% |
| surface normals | 0.00 ms | 0.0% | 0.00 ms | 0.0% |
| **forces (x2 per step)** | **445.47 ms** | **76.0%** | **134.50 ms** | **76.4%** |
| timestep control | 4.54 ms | 0.8% | 1.26 ms | 0.7% |
| integration | 4.39 ms | 0.7% | 1.81 ms | 1.0% |
| emitters + sinks | 1.19 ms | 0.2% | 1.04 ms | 0.6% |
| statistics | 2.24 ms | 0.4% | 0.85 ms | 0.5% |
| **total** | **586.21 ms** | | **176.08 ms** | |

Reading it:

- **Forces dominate at 76%**, and are charged for *both* evaluations per
  step (the predictor-corrector re-evaluates at the half step). Density is
  the same neighbour traversal doing less work per pair.
- **`surface normals: 0.000 ms`** is not a rounding artefact. The pass
  short-circuits when no material in the scenario has a non-zero
  surface-tension coefficient, so scenarios that do not need it pay
  literally nothing. Likewise `boundary volumes`, which is computed once
  at construction unless an obstacle is actually moving.
- **The serial fraction is small.** `linked-cell build` is the only stage
  with a serial component of any size, at 2.1% of a four-thread step.

### The profile was wrong first, and the wrong version was actionable

The first version of this table reported **integration at 27% of step
time** and summed to 260 ms/step against a measured 189 ms/step wall
clock. The integrator re-evaluates forces through a callback, so the stage
timers nested and the mid-step force evaluation was charged to both
columns. Worth recording because a 27% integration cost is exactly the
kind of number that sends an afternoon of optimisation effort in the wrong
direction; the real figure is 1%.

---

## What was optimised, and what was measured

Three changes, in the order the profile surfaced them. Same machine, same
scenario, 217,736 particles, 4 threads.

| stage | before | after |
|---|---:|---:|
| linked-cell build | 8.22 ms | **3.70 ms** |
| density + pressure | 36.90 ms | **32.93 ms** |
| forces | 140.43 ms | **134.50 ms** |
| integration | 1.69 ms (after unnesting) | **1.81 ms** |
| **total** | **190.22 ms** | **176.08 ms** |

Speedup at 4 threads vs 1 improved from 3.19x to 3.33x on this scenario.

**1. Integrator scratch buffers.** Two `std::vector<glm::vec3>` of the
full particle count were being allocated, zeroed and freed on *every
step* -- about 5 MB per step at this size, and more than half of it for
boundary indices the loops never touch. They are now reused members sized
to the fluid range.

**2. Linked cell: CSR instead of a bucket per cell.** `docs/architecture.md`
predicted, before any of this was threaded, that `build()` would become
the bottleneck once the O(N x neighbours) loops sped up. **The prediction
was directionally right and quantitatively not worth acting on**: build
was 4.3% of step time, so parallelising it for its own sake caps out at a
~3% total gain.

What the profile actually showed is that `vector<vector<int>>` made every
neighbour query dereference 27 independent heap pointers -- inside the
density and force loops, which are 95% of step time. The buckets are now
two flat arrays (a per-cell prefix sum plus one index array), built by
counting sort with nothing allocated per step, and the three x-adjacent
cells are contiguous in the flat index so a 27-cell gather is nine
contiguous copies rather than 27 pointer chases. That is why density and
forces got faster as well as build.

**The scatter pass is still deliberately serial.** The textbook parallel
counting sort claims slots with an atomic fetch-add, which is correct but
leaves each cell's contents in thread-scheduling order -- and a cell's
order *is* the summation order of every neighbour loop that later reads
it. Float addition is not associative, so that would silently make every
density and force sum thread-count-dependent. The pass is one array write
per particle with no allocation; keeping it ordered costs far less than
the guarantee is worth.

**3. Loop ranges skip boundary particles.** With a static schedule and
more boundary particles than fluid, the first thread's share was almost
entirely no-op iterations. Every parallel loop now starts at the first
fluid index.

---

## Determinism check (the correctness bar for parallelization)

FPS differing by thread count is expected and desired. What must *not*
differ is the physics.

v1 verified this by comparing reported statistics at checkpoints. **v2's
bar is higher**, because v2 introduced two things that could break it
invisibly: an adaptive timestep (a global reduction that feeds back into
every later step) and a particle count that changes during the run.

`tests/test_determinism.cpp` and `tests/test_dynamic_particles.cpp`
therefore assert **bit-identical particle state by `memcmp`** after 40
steps at **1, 2, 4 and 8 threads** -- with surface tension, XSPH, boundary
particles, emitters and sinks all active. 8 is included deliberately even
on a 4-core machine: oversubscription changes scheduling, which is exactly
what a fragile reduction is sensitive to. The dt sequence is compared
step-by-step as well, since a diverging dt would be the first symptom.

Three properties make it hold, each chosen against an easier alternative:

1. Parallel loops write only their own particle -- no reduction to
   reassociate.
2. Global reductions use fixed-size chunks independent of thread count
   (`core/ParallelReduce.hpp`), for sums as well as maxima.
3. Particle ordering is a pure function of scenario and step count:
   ordered emission, stable compaction, and the ordered linked-cell
   scatter above.

See `docs/experiments.md` for what the guarantee explicitly does *not*
cover (different compilers, flags, or architectures).

---

## Scenario cost

Every scenario at `--quality low`, four threads, **full duration**, from
`scripts/run_scenarios.sh low`. All fourteen report `STABLE` — zero
non-finite particles and zero particles outside a non-open domain face.

| scenario | fluid | boundary | steps | wall (s) |
|---|---:|---:|---:|---:|
| `coastal_wave` | 5,916 | 16,672 | 11,504 | 117 |
| `container_fill` | 1,924 | 5,744 | 8,865 | 21 |
| `controlled_wave_tank` | 6,880 | 9,738 | 5,292 | 55 |
| `dam_break` | 3,864 | 13,176 | 5,052 | 32 |
| `double_dam_break` | 7,727 | 16,416 | 5,878 | 71 |
| `droplet_impact` | 4,534 | 8,650 | 1,811 | 17 |
| `flash_flood` | 5,994 | 24,274 | 14,457 | 166 |
| `flood` | 11,037 | 18,635 | 9,903 | 200 |
| `fountain` | 11,822 | 24,730 | 10,948 | 233 |
| `obstacle_flow` | 4,761 | 7,259 | 8,950 | 55 |
| `sloshing_tank` | 2,448 | 5,016 | 15,911 | 68 |
| `spillway` | 5,546 | 9,002 | 9,188 | 67 |
| `tsunami_pulse` | 4,813 | 16,224 | 7,753 | 64 |
| `waterfall` | 1,960 | 22,132 | 11,320 | 42 |

The whole suite is about 20 minutes at `low`. Useful for picking a
resolution: `medium` is roughly 6x the particles and rather more than 6x
the time (a finer `h` shortens the CFL step too, so step count rises as
well as step cost); `high` is another 6x on top.

Two things this table shows that a per-scenario timing does not:

- **Boundary particles outnumber fluid in six of the fourteen.** That is
  inherent to shallow, wide geometry: boundary count scales with surface
  area over spacing squared, fluid count with volume over spacing cubed,
  so the ratio improves as resolution rises. `waterfall` is the extreme —
  22,132 boundary particles for 1,960 fluid, because the tall ledge and
  the deep domain are almost all wall.
- **Step count varies more than particle count.** `sloshing_tank` runs
  15,911 steps for 2,448 particles because it simulates 9 seconds;
  `droplet_impact` runs 1,811 for 4,534 because it simulates 0.12. Wall
  time is the product, and the adaptive timestep means neither factor is
  knowable from the geometry alone.

Re-run `scripts/run_scenarios.sh medium results/medium` for figures on
your own machine rather than trusting these.

---

## Stability validation

Every scenario in `configs/scenarios/` runs to completion and reports
`STABLE` -- zero non-finite particles and zero particles outside a
non-open domain face -- at every quality preset tested.
`scripts/run_scenarios.sh` exits non-zero if any does not, so it works
directly as a CI gate. Quantitative comparison against analytical results
is in [`docs/validation.md`](../docs/validation.md).
