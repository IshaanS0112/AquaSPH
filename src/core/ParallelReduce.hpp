#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

namespace aquasph {

// DETERMINISTIC PARALLEL REDUCTIONS.
//
// The project's headline guarantee is bit-identical output across thread
// counts. Up to now that came for free: every parallel loop wrote only
// particles[i]'s own fields, so no reduction existed to reassociate.
// Adaptive timestepping breaks that -- dt is a global function of
// max|v| and max|a| over all particles, and it feeds straight back into
// the integration, so a reduction that varies with thread count would
// silently make the *whole simulation* thread-dependent. Scenario
// metrics (fluid volume, mean density, inundated area) have the same
// property once they are asserted on in CI.
//
// Why not `#pragma omp parallel for reduction(max:)` / `reduction(+:)`:
//
//   * `+` genuinely reassociates. OpenMP is free to combine per-thread
//     partials in any order, and float addition is not associative, so
//     the last bits of a sum over 200k particles legitimately change
//     with thread count. This is the real hazard.
//   * `max` over finite floats *is* order-independent (it selects an
//     input value; no rounding occurs), so `reduction(max:)` would in
//     fact be reproducible today. It stops being reproducible the moment
//     a NaN enters the array, and it offers no protection against the
//     next reduction someone adds being a sum.
//
// Rather than reason about that distinction at every call site, both
// reductions below are made thread-count-independent *by construction*:
// the index range is split into a fixed number of chunks whose size does
// not depend on the thread count, each chunk is folded serially in index
// order, and the chunk partials are then folded serially in chunk order.
// The arithmetic performed is therefore a pure function of n and the
// data -- OpenMP only decides *which core* evaluates each chunk, never
// *how the results combine*. tests/test_determinism.cpp asserts this at
// 1, 2, 4 and 8 threads.
//
// Cost: one std::vector<T> of size ceil(n/kChunk) per call (~50 doubles
// at 200k particles) and one serial fold over it. Negligible next to the
// O(N * neighbours) loops these run alongside.
namespace reduce {

// Elements folded serially per chunk. Chosen so that (a) the serial
// fold over partials stays trivially short even at millions of
// particles, and (b) chunks are large enough that per-chunk overhead
// disappears. Deliberately a compile-time constant: making it depend on
// omp_get_max_threads() would reintroduce exactly the thread-count
// dependence this file exists to remove.
constexpr int kChunk = 4096;

inline int chunkCount(int n) { return n <= 0 ? 0 : (n + kChunk - 1) / kChunk; }

// max_i f(i), or `identity` when n == 0. NaN-safe in the sense that the
// result is the same regardless of thread count (a NaN is compared the
// same way in the same order every time).
template <typename F>
float deterministicMax(int n, float identity, F&& f) {
    const int chunks = chunkCount(n);
    if (chunks == 0) return identity;
    std::vector<float> partial(static_cast<size_t>(chunks), identity);

    #pragma omp parallel for schedule(static)
    for (int c = 0; c < chunks; ++c) {
        const int begin = c * kChunk;
        const int end = std::min(begin + kChunk, n);
        float m = identity;
        for (int i = begin; i < end; ++i) m = std::max(m, f(i));
        partial[static_cast<size_t>(c)] = m;
    }

    float result = identity;
    for (float v : partial) result = std::max(result, v);
    return result;
}

// sum_i f(i), accumulated in double regardless of f's return type.
// Chunk partials are folded in ascending chunk order, so the exact
// sequence of additions is fixed by n alone.
template <typename F>
double deterministicSum(int n, F&& f) {
    const int chunks = chunkCount(n);
    if (chunks == 0) return 0.0;
    std::vector<double> partial(static_cast<size_t>(chunks), 0.0);

    #pragma omp parallel for schedule(static)
    for (int c = 0; c < chunks; ++c) {
        const int begin = c * kChunk;
        const int end = std::min(begin + kChunk, n);
        double s = 0.0;
        for (int i = begin; i < end; ++i) s += static_cast<double>(f(i));
        partial[static_cast<size_t>(c)] = s;
    }

    double result = 0.0;
    for (double v : partial) result += v;
    return result;
}

// count_i [pred(i)] -- integer, so ordering cannot matter arithmetically,
// but kept here so call sites use one consistent idiom.
template <typename F>
long long deterministicCount(int n, F&& pred) {
    const int chunks = chunkCount(n);
    if (chunks == 0) return 0;
    std::vector<long long> partial(static_cast<size_t>(chunks), 0);

    #pragma omp parallel for schedule(static)
    for (int c = 0; c < chunks; ++c) {
        const int begin = c * kChunk;
        const int end = std::min(begin + kChunk, n);
        long long k = 0;
        for (int i = begin; i < end; ++i) if (pred(i)) ++k;
        partial[static_cast<size_t>(c)] = k;
    }

    long long result = 0;
    for (long long v : partial) result += v;
    return result;
}

} // namespace reduce
} // namespace aquasph
