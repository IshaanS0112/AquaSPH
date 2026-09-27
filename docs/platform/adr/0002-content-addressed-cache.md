# ADR-0002: Content-addressed result cache

**Status:** accepted

## Context
Parameter studies resubmit the same configuration often: re-running a sweep
after adding one grid point, or a notebook re-executing its cells. A
medium-quality run costs about a minute of a full machine.

## Decision
Key results on `sha256(spec_hash + ":" + solver_id)`, where `spec_hash`
covers the resolved scenario and every run parameter that affects results,
and `solver_id` is the SHA-256 of the solver binary.

## Why this is sound here, and would not be in general
Caching simulation output is only correct if the same input produces the same
output. AquaSPH guarantees this bit-for-bit across 1, 2, 4, and 8 threads
(`tests/test_determinism.cpp`, and the deterministic fixed-chunk reductions in
`src/core/ParallelReduce.hpp`). Thread count is therefore excluded from the
key. Without that guarantee, a cache would silently return one of many
possible answers.

## Rejected alternatives
| Option | Why not |
|---|---|
| Key on a solver version string | Someone forgets to bump it, and stale physics gets served. Hashing the binary cannot be forgotten. |
| Key on git SHA | Two builds of one commit with different compiler flags are different solvers. `-ffast-math` would break determinism, for example. |
| Cache in Redis | The result must outlive Redis restarts, and it is already stored in the job row. |

## Consequences
- A cache hit creates a new job row (the tenant owns it) with
  `cache_hit=true` and `source_job_id`, and it shares the source job's
  artifacts. There is no copy.
- Default scope is per-tenant. `global` is opt-in because of the timing side
  channel described in TRD §4.
