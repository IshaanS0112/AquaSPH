#include "LinkedCell.hpp"
#include <algorithm>
#include <cmath>

namespace aquasph {

LinkedCell::LinkedCell(const glm::vec3& domainMin, const glm::vec3& domainMax, float cellSize)
    : domainMin_(domainMin), domainMax_(domainMax), cellSize_(cellSize) {
    const glm::vec3 span = domainMax_ - domainMin_;
    gridDims_.x = std::max(1, static_cast<int>(std::floor(span.x / cellSize_)) + 1);
    gridDims_.y = std::max(1, static_cast<int>(std::floor(span.y / cellSize_)) + 1);
    gridDims_.z = std::max(1, static_cast<int>(std::floor(span.z / cellSize_)) + 1);
    cells_.resize(static_cast<size_t>(gridDims_.x) * gridDims_.y * gridDims_.z);
}

glm::ivec3 LinkedCell::cellCoords(const glm::vec3& pos) const {
    const glm::vec3 rel = (pos - domainMin_) / cellSize_;
    glm::ivec3 c;
    c.x = std::clamp(static_cast<int>(std::floor(rel.x)), 0, gridDims_.x - 1);
    c.y = std::clamp(static_cast<int>(std::floor(rel.y)), 0, gridDims_.y - 1);
    c.z = std::clamp(static_cast<int>(std::floor(rel.z)), 0, gridDims_.z - 1);
    return c;
}

int LinkedCell::flatten(const glm::ivec3& c) const {
    return c.x + gridDims_.x * (c.y + gridDims_.y * c.z);
}

// DELIBERATELY SERIAL, even though computeDensity/
// ForceCompute. Naively parallelizing this loop would be a real data
// race: multiple threads could land on the same cell and call
// push_back() on its bucket concurrently (a std::vector isn't safe for
// concurrent mutation, and reallocation during one thread's push_back
// would invalidate the pointers another thread is mid-write on). Making
// it parallel correctly means restructuring to a two-pass counting-sort
// (parallel per-cell atomic counts, then a prefix sum, then a parallel
// scatter using atomic fetch-add for each particle's slot) -- a real
// change, not a one-line pragma, and not the highest-value place to
// spend that effort: build() is O(N) with cheap work per particle,
// while computeDensityPressure/computeForces are O(N * neighbors) with
// much more work per particle. Worth revisiting if profiling ever shows
// build() has become the bottleneck now that the O(N*neighbors) loops
// are threaded (Amdahl's law: as the parallel parts get faster, this
// serial part's share of total time grows).
void LinkedCell::build(const std::vector<Particle>& particles) {
    for (auto& bucket : cells_) bucket.clear();
    for (int i = 0; i < static_cast<int>(particles.size()); ++i) {
        const glm::ivec3 c = cellCoords(particles[i].position);
        cells_[flatten(c)].push_back(i);
    }
}

void LinkedCell::getNeighbors(int particleIdx, const std::vector<Particle>& particles,
                               std::vector<int>& outNeighbors) const {
    const glm::ivec3 c = cellCoords(particles[particleIdx].position);
    for (int dz = -1; dz <= 1; ++dz) {
        const int z = c.z + dz;
        if (z < 0 || z >= gridDims_.z) continue;
        for (int dy = -1; dy <= 1; ++dy) {
            const int y = c.y + dy;
            if (y < 0 || y >= gridDims_.y) continue;
            for (int dx = -1; dx <= 1; ++dx) {
                const int x = c.x + dx;
                if (x < 0 || x >= gridDims_.x) continue;
                const auto& bucket = cells_[flatten(glm::ivec3(x, y, z))];
                outNeighbors.insert(outNeighbors.end(), bucket.begin(), bucket.end());
            }
        }
    }
}

} // namespace aquasph
