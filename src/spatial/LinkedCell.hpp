#pragma once
#include <vector>
#include <glm/glm.hpp>
#include "../core/Particle.hpp"

namespace aquasph {

// Uniform grid spatial hash ("linked-cell" method). The domain is divided
// into cubic cells of side length `cellSize` (== smoothing radius h), so
// any particle within kernel support of another lies in that particle's
// own cell or one of its 26 face/edge/corner-adjacent cells -- 27 cells
// total in 3D. This reduces neighbor search from O(N^2) (naive all-pairs)
// to O(N) amortized (each particle only checks a constant number of
// cells, and cells hold ~O(1) particles on average for a roughly uniform
// distribution).
//
// STORAGE: COMPRESSED-SPARSE-ROW, NOT A VECTOR OF VECTORS.
//
// The original implementation kept `std::vector<std::vector<int>>`, one
// heap-allocated bucket per cell. Profiling at 218k particles (see
// benchmarks/scaling_results.md) said two things about that:
//
//  * build() spent most of its time clearing 112,000 separate vector
//    objects every step, not on the particles themselves;
//  * and, far more importantly, every neighbour query dereferenced 27
//    independent heap pointers scattered across memory -- inside the
//    density and force loops, which together are ~93% of step time.
//
// So the buckets are now two flat arrays: `cellStart_` (a prefix sum, one
// entry per cell plus a terminator) and `indices_` (every particle index,
// grouped by cell). A cell's contents are the contiguous slice
// indices_[cellStart_[c] .. cellStart_[c+1]). Nothing is allocated per
// step once the arrays have grown, and a 27-cell gather walks 27 short
// contiguous runs instead of chasing pointers.
//
// DETERMINISM. Particle indices stay in ascending order within each cell,
// because the scatter pass walks particles in index order. That is not
// cosmetic: the order of a cell's contents is the order in which its
// particles are summed in every neighbour loop, and float addition is not
// associative, so a scatter using atomic fetch-add cursors -- the obvious
// way to parallelise this pass -- would make every density and force sum
// depend on thread scheduling. That is exactly the guarantee this project
// exists to keep, so the scatter stays ordered.
class LinkedCell {
public:
    LinkedCell(const glm::vec3& domainMin, const glm::vec3& domainMax, float cellSize);

    // Rebuilds the grid from current particle positions. Must be called
    // once per timestep before any getNeighbors() calls, since particles
    // move between steps. Handles a changing particle count (emitters and
    // sinks) without any special case.
    void build(const std::vector<Particle>& particles);

    // Appends candidate neighbor indices (including particleIdx itself,
    // via its own cell) to outNeighbors, drawn from the particle's cell
    // and its 26 neighboring cells. This is a candidate set, not an
    // exact-radius query: callers must still check |r_ij| < h, since two
    // particles in diagonally adjacent cells can be up to
    // sqrt(3)*cellSize apart.
    void getNeighbors(int particleIdx, const std::vector<Particle>& particles,
                       std::vector<int>& outNeighbors) const;

    glm::ivec3 dims() const { return gridDims_; }
    int cellCount() const { return gridDims_.x * gridDims_.y * gridDims_.z; }

private:
    glm::vec3 domainMin_;
    glm::vec3 domainMax_;
    float cellSize_;
    glm::ivec3 gridDims_;

    std::vector<int> cellStart_;   // cellCount() + 1 entries
    std::vector<int> indices_;     // one entry per particle, grouped by cell
    std::vector<int> cellOf_;      // scratch: each particle's flat cell index
    std::vector<int> cursor_;      // scratch: per-cell write position
    int count_ = 0;                // particles in the current build

    glm::ivec3 cellCoords(const glm::vec3& pos) const;
    int flatten(const glm::ivec3& c) const;
};

} // namespace aquasph
