# AquaSPH -- Benchmark Results (Phase 0 + Phase 1 OpenMP)

All numbers below are measured, not estimated -- captured from real runs
of `./aquasph --particles N --steps S --threads T --quiet` in Release
mode, same build, same machine (a 4-core sandbox VM -- see hardware note
below), same session, immediately after a clean rebuild.

## Multi-thread results

| Particle Count | 1 Thread | 2 Threads | 4 Threads | 8 Threads | Speedup (4T vs 1T) |
|---|---|---|---|---|---|
| 5,832  | 136.43 FPS (7.330 ms/step)  | 252.55 FPS (3.960 ms/step) | 476.74 FPS (2.098 ms/step) | 320.74 FPS (3.118 ms/step) | 3.49x |
| 10,648 | 34.30 FPS (29.157 ms/step)  | 64.95 FPS (15.397 ms/step) | 118.51 FPS (8.438 ms/step) | 111.39 FPS (8.978 ms/step) | 3.46x |
| 27,000 | 5.32 FPS (188.024 ms/step)  | 10.19 FPS (98.137 ms/step) | 16.86 FPS (59.330 ms/step) | 16.35 FPS (61.173 ms/step) | 3.17x |
| 50,653 | 1.36 FPS (734.266 ms/step)  | 2.64 FPS (378.374 ms/step) | 4.13 FPS (241.929 ms/step) | 4.08 FPS (245.154 ms/step) | 3.03x |

(Particle counts aren't exactly 5k/10k/25k/50k for the same reason as
Phase 0: `--particles N` targets N via a cubic lattice, the actual count
is whichever perfect cube comes closest, and the program always prints
the real count it used.)

## Hardware: 4 physical cores, not 8

This sandbox reports `nproc` = 4. That's why the table above has an
8-thread column but it's never the fastest one: 8 threads on 4 physical
cores is oversubscription, not parallelism -- the OS is now
time-slicing 8 software threads across 4 hardware cores, which adds
scheduling/context-switch overhead without adding any actual compute
capacity. The data shows exactly that: 8T is flat-to-worse than 4T at
every particle count tested, most visibly at 5,832 particles (320.74
FPS at 8T vs 476.74 FPS at 4T -- worse by 33%). This is expected,
textbook behavior, not a bug -- reported honestly rather than only
showing the flattering columns. On real 8-core hardware, expect the
8-thread column to look like the 4-thread column does here (further
speedup, not regression) -- reproduce with `nproc` on your own machine
and adjust `--threads` accordingly.

## Parallel efficiency: 87% at 5.8k particles, down to 76% at 50.6k

Speedup at 4 threads (vs. 1 thread): 3.49x, 3.46x, 3.17x, 3.03x, for
5,832 / 10,648 / 27,000 / 50,653 particles respectively -- efficiency
(speedup / 4) declining from ~87% to ~76% as N grows. That's the
*opposite* of the usual "parallel efficiency improves at scale" pattern
(where fixed per-step overhead like OpenMP's parallel-region spawn/join
amortizes better over more work). A plausible explanation: this project
uses an AoS particle layout (see `docs/architecture.md`), so each thread
pulls position **and** velocity **and** force **and** density **and**
pressure **and** mass into cache for every particle touched, even when a
given loop only needs two or three of those fields. As N grows past
cache capacity, four threads are competing harder for the same memory
bandwidth, which would show up exactly as declining efficiency at larger
N -- consistent with what's measured, though this is a hypothesis, not
something confirmed with an actual profiler (`perf stat`, cache-miss
counters). If it holds up, it's a concrete, measured reason to revisit
the AoS-vs-SoA tradeoff in a later phase, rather than a vague "SoA is
usually faster" argument.

## Determinism check (the actual correctness bar for Phase 1)

FPS differing by thread count is expected and desired. What must *not*
differ is the physics: parallelizing `computeDensityPressure` and
`computeForces` over particles is only safe because each iteration
writes exactly one particle's own fields and reads (without mutating)
its neighbors' -- see the comments in `src/core/DensityPressure.cpp` and
`src/core/ForceCompute.cpp` for the full reasoning, including the
data-race that a naive parallelization would have hit (a single shared
`neighbors` buffer reused across iterations, safe only when there was
one thread).

Verified directly: ran the same 8,000-particle dam-break scenario at 1,
2, and 4 threads for 250 steps, comparing density min/avg/max, minimum
Y-position, and max particle speed at every 50-step checkpoint. All
values matched exactly across all three thread counts at every
checkpoint -- not just "close," identical. This is expected given the
parallelization only threads the *outer* per-particle loop; the *inner*
per-neighbor summation for any given particle still runs sequentially
within a single thread regardless of how many threads exist overall, so
there's no floating-point reassociation from parallelism -- each
particle's result is computed via the exact same instruction sequence
no matter the thread count.

## What's still serial, and why

`LinkedCell::build()` is not parallelized. Multiple threads could land
particles in the same cell and call `push_back()` on its bucket
concurrently -- a real data race (concurrent mutation of a
`std::vector`, plus the possibility of one thread's push_back()
reallocating the buffer out from under another thread's write).
Parallelizing it correctly means restructuring to a two-pass
counting-sort (parallel atomic per-cell counts, a prefix sum, then a
parallel scatter using atomic fetch-add for each particle's slot) --
real additional work, and not the highest-value target: `build()` is
O(N) with cheap per-particle work, while the two parallelized loops are
O(N * neighbors) with far more work per particle. Worth revisiting if
profiling ever shows `build()` has become the bottleneck now that the
O(N*neighbors) loops are faster (Amdahl's law: as the parallel parts
speed up, whatever's still serial becomes a larger share of total time).

## How to reproduce

```bash
cd build
./aquasph --particles 5000  --steps 150 --threads 1 --quiet
./aquasph --particles 5000  --steps 150 --threads 4 --quiet
# ...repeat for 10000/25000/50000 and other thread counts.
# nproc tells you your machine's physical core count -- that's the
# thread count where you should expect the best result, not necessarily
# the highest number you can pass.
```

## Stability validation (the actual Phase 0/1 success criterion)

Separately from the throughput sweep above, the default scenario
(`configs/default.json`, ~8,000-9,300 particles depending on exact
lattice rounding) was run for 1,200+ timesteps at both 1 and 4 threads
and stayed STABLE throughout (zero NaN / zero out-of-domain particles at
every checkpoint, identical physics at both thread counts per the
determinism check above). See `docs/architecture.md` for the full
stability-tuning story.
