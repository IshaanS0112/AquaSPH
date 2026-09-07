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
    cellStart_.assign(static_cast<size_t>(cellCount()) + 1, 0);
    cursor_.assign(static_cast<size_t>(cellCount()), 0);
}

glm::ivec3 LinkedCell::cellCoords(const glm::vec3& pos) const {
    const glm::vec3 rel = (pos - domainMin_) / cellSize_;
    glm::ivec3 c;
    // Non-finite coordinates land in cell 0 rather than propagating into
    // an out-of-range index: a diverging simulation must still be able to
    // finish its step and report UNSTABLE instead of reading out of
    // bounds on the way there.
    c.x = std::isfinite(rel.x) ? std::clamp(static_cast<int>(std::floor(rel.x)), 0, gridDims_.x - 1) : 0;
    c.y = std::isfinite(rel.y) ? std::clamp(static_cast<int>(std::floor(rel.y)), 0, gridDims_.y - 1) : 0;
    c.z = std::isfinite(rel.z) ? std::clamp(static_cast<int>(std::floor(rel.z)), 0, gridDims_.z - 1) : 0;
    return c;
}

int LinkedCell::flatten(const glm::ivec3& c) const {
    return c.x + gridDims_.x * (c.y + gridDims_.y * c.z);
}

// COUNTING SORT, in three passes.
//
// Pass 1 (parallel) assigns each particle its flat cell index. Every
// iteration writes only cellOf_[i], so it is embarrassingly parallel.
//
// Pass 2 (parallel count, serial prefix sum) turns per-cell occupancy
// into the CSR start offsets. The counting increments are integers, so
// their result does not depend on the order the atomics happen to land
// in; the prefix sum is a sequential scan over cells and is cheap
// (a few hundred thousand adds at the resolutions this project runs).
//
// Pass 3 (SERIAL, deliberately) scatters particle indices into their
// cells in ascending particle order. The textbook parallel version uses
// an atomic fetch-add per particle to claim a slot, which is correct but
// leaves each cell's contents in thread-scheduling order -- and a cell's
// order is the summation order of every neighbour loop that later reads
// it. Float addition is not associative, so that would silently make
// every density and force sum thread-count-dependent. The pass is one
// array write per particle with no allocation; keeping it ordered costs
// far less than the guarantee is worth.
void LinkedCell::build(const std::vector<Particle>& particles) {
    const int n = static_cast<int>(particles.size());
    const int cells = cellCount();

    if (static_cast<int>(cellOf_.size()) < n) {
        cellOf_.resize(static_cast<size_t>(n));
        indices_.resize(static_cast<size_t>(n));
    }

    #pragma omp parallel for schedule(static)
    for (int i = 0; i < n; ++i) {
        cellOf_[static_cast<size_t>(i)] = flatten(cellCoords(particles[i].position));
    }

    std::fill(cellStart_.begin(), cellStart_.end(), 0);
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < n; ++i) {
        #pragma omp atomic
        ++cellStart_[static_cast<size_t>(cellOf_[static_cast<size_t>(i)]) + 1];
    }

    for (int c = 0; c < cells; ++c) {
        cellStart_[static_cast<size_t>(c) + 1] += cellStart_[static_cast<size_t>(c)];
    }

    std::copy(cellStart_.begin(), cellStart_.begin() + cells, cursor_.begin());
    for (int i = 0; i < n; ++i) {
        indices_[static_cast<size_t>(cursor_[static_cast<size_t>(cellOf_[static_cast<size_t>(i)])]++)] = i;
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
            // The three cells along x are contiguous in the flat index, so
            // one slice covers all of them: their combined range is
            // [start(x-1), end(x+1)). That turns the innermost of the
            // three loops into a single contiguous copy.
            const int x0 = std::max(c.x - 1, 0);
            const int x1 = std::min(c.x + 1, gridDims_.x - 1);
            const int base = gridDims_.x * (y + gridDims_.y * z);
            const int begin = cellStart_[static_cast<size_t>(base + x0)];
            const int end = cellStart_[static_cast<size_t>(base + x1) + 1];
            if (end <= begin) continue;
            outNeighbors.insert(outNeighbors.end(),
                                 indices_.begin() + begin, indices_.begin() + end);
        }
    }
}

} // namespace aquasph
