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
class LinkedCell {
public:
    LinkedCell(const glm::vec3& domainMin, const glm::vec3& domainMax, float cellSize);

    // Rebuilds the grid from current particle positions. Must be called
    // once per timestep before any getNeighbors() calls, since particles
    // move between steps.
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
    std::vector<std::vector<int>> cells_;

    glm::ivec3 cellCoords(const glm::vec3& pos) const;
    int flatten(const glm::ivec3& c) const;
};

} // namespace aquasph
