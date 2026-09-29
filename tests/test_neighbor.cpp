#include <gtest/gtest.h>
#include <algorithm>
#include "core/Particle.hpp"
#include "spatial/LinkedCell.hpp"

using namespace aquasph;

namespace {
std::vector<Particle> makeLattice(int n, float spacing) {
    std::vector<Particle> particles;
    particles.reserve(static_cast<size_t>(n) * n * n);
    for (int ix = 0; ix < n; ++ix)
        for (int iy = 0; iy < n; ++iy)
            for (int iz = 0; iz < n; ++iz) {
                Particle p;
                p.position = glm::vec3(static_cast<float>(ix), static_cast<float>(iy), static_cast<float>(iz)) * spacing
                             + glm::vec3(spacing * 0.5f);
                particles.push_back(p);
            }
    return particles;
}
} // namespace

TEST(LinkedCell, InteriorParticleSeesFullTwentySevenCellBlock) {
    // 7^3 lattice with spacing == cellSize puts exactly one particle per cell, so the particle
    // at the lattice center should see exactly 27 candidates (its own cell + all 26 neighbors,
    // none clipped by the domain boundary).
    const int n = 7;
    const float spacing = 0.1f;
    auto particles = makeLattice(n, spacing);

    LinkedCell grid(glm::vec3(0.0f), glm::vec3(static_cast<float>(n) * spacing), spacing);
    grid.build(particles);

    const int midIdx = (n / 2) * n * n + (n / 2) * n + (n / 2);
    std::vector<int> neighbors;
    grid.getNeighbors(midIdx, particles, neighbors);

    EXPECT_EQ(static_cast<int>(neighbors.size()), 27);
}

TEST(LinkedCell, CornerParticleSeesFewerCandidatesThanInterior) {
    const int n = 7;
    const float spacing = 0.1f;
    auto particles = makeLattice(n, spacing);

    LinkedCell grid(glm::vec3(0.0f), glm::vec3(static_cast<float>(n) * spacing), spacing);
    grid.build(particles);

    std::vector<int> cornerNeighbors;
    grid.getNeighbors(0, particles, cornerNeighbors); // particle at (0,0,0)

    const int midIdx = (n / 2) * n * n + (n / 2) * n + (n / 2);
    std::vector<int> midNeighbors;
    grid.getNeighbors(midIdx, particles, midNeighbors);

    EXPECT_LT(cornerNeighbors.size(), midNeighbors.size());
    EXPECT_EQ(static_cast<int>(cornerNeighbors.size()), 8); // only the 2x2x2 block is in-bounds
}

TEST(LinkedCell, CandidateSetIncludesSelf) {
    const int n = 3;
    const float spacing = 0.1f;
    auto particles = makeLattice(n, spacing);
    LinkedCell grid(glm::vec3(0.0f), glm::vec3(static_cast<float>(n) * spacing), spacing);
    grid.build(particles);

    std::vector<int> neighbors;
    grid.getNeighbors(0, particles, neighbors);
    EXPECT_NE(std::find(neighbors.begin(), neighbors.end(), 0), neighbors.end());
}

TEST(LinkedCell, RebuildReflectsMovedParticles) {
    const int n = 3;
    const float spacing = 0.1f;
    auto particles = makeLattice(n, spacing);
    LinkedCell grid(glm::vec3(0.0f), glm::vec3(static_cast<float>(n) * spacing), spacing);
    grid.build(particles);

    // Move particle 0 away, rebuild, and check the neighbours of particle 1 no longer include it.
    particles[0].position = glm::vec3(static_cast<float>(n) * spacing - 0.01f);
    grid.build(particles);

    std::vector<int> neighbors;
    grid.getNeighbors(1, particles, neighbors);
    EXPECT_EQ(std::find(neighbors.begin(), neighbors.end(), 0), neighbors.end());
}
