#pragma once
#include <vector>
#include <glm/glm.hpp>
#include "../core/Particle.hpp"

namespace aquasph {

// Uniform grid spatial hash ("linked-cell" method).
class LinkedCell {
public:
    LinkedCell(const glm::vec3& domainMin, const glm::vec3& domainMax, float cellSize);

    // Rebuilds the grid from current particle positions.
    void build(const std::vector<Particle>& particles);

    // Appends candidate neighbor indices (including particleIdx itself, via its own cell) to
    // outNeighbors, drawn from the particle's cell and its 26 neighboring cells.
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

    glm::ivec3 cellCoords(const glm::vec3& pos) const;
    int flatten(const glm::ivec3& c) const;
};

} // namespace aquasph
