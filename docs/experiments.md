# Reproducibility

Every result this project publishes has to be re-derivable from what is in
this repository. That means each recorded experiment carries the scenario,
the code revision, the resolution, the numerics, the thread count and the
runtime — and that the same inputs give the same outputs.

---

## What a run records

`--metrics out.json` writes a machine-readable record. Its provenance
block answers "what produced this?" without reference to anything outside
the file:

```jsonc
{
  "scenario": "dam_break",
  "tier": "Tier 1 (physically demonstrable)",
  "tier_caveat": "Resolves the named phenomenon at this particle count; ...",
  "approximation": "Idealised initial condition ... See docs/validation.md.",
  "git_revision": "7d18291",           // stamped in at CMake configure time
  "quality": "medium",
  "threads": 4,
  "status": "STABLE",

  "particles":  { "fluid_final": ..., "fluid_peak": ..., "boundary": ...,
                   "emitted_total": ..., "removed_total": ..., "hit_ceiling": false },
  "resolution": { "smoothing_radius": ..., "spacing": ... },
  "time":       { "simulated_seconds": ..., "steps": ..., "wall_seconds": ...,
                   "avg_step_ms": ..., "dt_min": ..., "dt_mean": ..., "dt_max": ... },
  "density":    { "min": ..., "max": ..., "avg_final": ...,
                   "fraction_within_1pct_final": ..., "fraction_within_1pct_mean": ... },
  "dynamics":   { "max_speed": ..., "max_acceleration": ...,
                   "containment_events": ..., "unstable_particles": 0 },
  "volume":     { "initial_m3": ..., "final_m3": ... },

  "surge_front":      [ { "t": ..., "front": ..., "Z": ..., "T": ... }, ... ],
  "probes":           [ { "name": ..., "position": [...],
                           "measured": { "amplitude": ..., "period": ..., ... },
                           "series": { "t": [...], "elevation": [...] } } ],
  "wave_predictions": [ { "valid": true, "height": ..., "wavelength": ..., ... } ],
  "inundation":       { "area_fraction": ..., "max_depth": ... }
}
```

The remaining inputs — domain, materials, regions, emitters, obstacles,
forces, numerics — live in the scenario file, which is version-controlled
next to the code. `git_revision` plus `configs/scenarios/<name>.json` is
therefore a complete description of the run.

Non-finite values are written as JSON `null` rather than `NaN`, which is
not valid JSON and would make the file unparseable by every standard
reader exactly when something has gone wrong and you most need to read it.

---

## Determinism

**Output is bit-identical across thread counts.** Not "close" — identical.
`tests/test_determinism.cpp` asserts it by `memcmp` over the whole
particle array after 40 steps at 1, 2, 4 and 8 threads, and
`tests/test_dynamic_particles.cpp` repeats it with emitters and sinks
active, where the particle count is changing during the run.

Three things make that true, and each was a deliberate choice against an
easier alternative:

1. **The parallel loops write only their own particle.** No reduction, no
   atomics, nothing to reassociate.
2. **The global reductions use fixed-size chunks**
   (`core/ParallelReduce.hpp`). The adaptive timestep is a global maximum
   over all particles that feeds straight back into every later step, so a
   thread-dependent reduction would make the whole trajectory
   thread-dependent — silently, and in a way no per-particle test would
   catch. The index range is split into chunks whose size does not depend
   on the thread count, each folded serially in index order, then the
   chunk partials folded in chunk order. OpenMP decides which core
   evaluates each chunk, never how the results combine.
3. **Particle ordering is a pure function of the scenario and the step
   count.** Emitters append in (emitter, lattice site) order; sinks
   compact stably; and `LinkedCell`'s scatter pass is deliberately serial
   and index-ordered. The textbook parallel counting sort claims slots
   with atomic fetch-add, which leaves each cell's contents in
   thread-scheduling order — and a cell's order *is* the summation order
   of every neighbour loop that reads it.

What determinism does **not** promise: identical results across different
compilers, optimisation levels, or CPU architectures. Different
instruction selection (FMA contraction, vectorisation width, `libm`
implementations) legitimately changes the last bits. The guarantee is
"same binary, same inputs, any thread count".

---

## Reproducing the published numbers

All of these run from the repository root against a Release build.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DAQUASPH_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### The scenario library

```bash
scripts/run_scenarios.sh medium results/medium
```

Runs every scenario, prints a STABLE/UNSTABLE table, and writes one
metrics JSON and one log per scenario. Exit code is non-zero if any
scenario is unstable, so it works directly as a CI gate.

### Throughput and scaling

```bash
scripts/benchmark.sh 40 dam_break
```

Sweeps quality × thread count and reports exclusive per-stage timings.
`benchmarks/scaling_results.md` is this output, with the machine
described. Re-run it on your own hardware rather than trusting the table:
`nproc` tells you where to expect the best thread count.

### Validation

```bash
scripts/run_validation.sh medium results/validation
python3 scripts/validate.py results/validation
```

Runs the three scenarios with quantitative references, compares them
against analytical results, and regenerates `docs/_validation_tables.md`
and the surge-front plot. See `docs/validation.md`.

### Gallery and clips

```bash
xvfb-run -s "-screen 0 1600x900x24" scripts/make_gallery.sh medium
```

Renders one clip per scenario plus a contact sheet. Needs a display (or
Xvfb) and ffmpeg.

---

## Stating results honestly

Rules this project holds itself to, and that anyone quoting its output
should hold to as well:

- **Every published figure states particle count, hardware, thread count,
  render mode, resolution, and whether it was offline or interactive.**
  The viewer prints all of that in its render summary for exactly this
  reason. An offline render is never described as real-time.
- **Tier is never dropped.** A Tier 2 scenario's numbers are internally
  consistent measurements of that simulation and are not predictions. A
  flood's inundation area is not a flood map; a tsunami pulse's run-up is
  not a hazard estimate.
- **Physical parameters are not tuned to make a validation curve agree.**
  Where the simulation and the reference disagree,
  `docs/validation.md` says by how much and gives the reason. A documented,
  explained discrepancy is worth far more than a suspiciously perfect fit.
- **Measured, not estimated.** Every number in `benchmarks/` and in the
  performance sections of `docs/architecture.md` came from a run on the
  machine described there. Where something was reasoned about but not
  measured, it says so.
