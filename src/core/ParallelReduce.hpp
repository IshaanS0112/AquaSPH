#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

namespace aquasph {

// Deterministic parallel reductions: the range is split into fixed-size chunks folded in index
// order, so results are bit-identical at any thread count.
namespace reduce {

// Elements folded serially per chunk.
constexpr int kChunk = 4096;

inline int chunkCount(int n) { return n <= 0 ? 0 : (n + kChunk - 1) / kChunk; }

// max_i f(i), or `identity` when n == 0.
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
