#include "BoundaryVolume.hpp"
#include "../spatial/LinkedCell.hpp"
#include <algorithm>

namespace aquasph {

void computeBoundaryVolumes(std::vector<Particle>& particles,
                             const LinkedCell& grid,
                             const CubicSplineKernel& kernel,
                             int boundaryEnd) {
    const float h = kernel.h();
    const int all = static_cast<int>(particles.size());
    const int n = (boundaryEnd < 0) ? all : std::min(boundaryEnd, all);

    #pragma omp parallel
    {
        std::vector<int> neighbors;
        neighbors.reserve(128);

        #pragma omp for schedule(static)
        for (int i = 0; i < n; ++i) {
            Particle& pi = particles[i];
            if (pi.kind != ParticleKind::Boundary) continue;

            neighbors.clear();
            grid.getNeighbors(i, particles, neighbors);

            float kernelSum = 0.0f;
            for (int j : neighbors) {
                if (particles[j].kind != ParticleKind::Boundary) continue;
                const float r = glm::length(pi.position - particles[j].position);
                if (r >= h) continue;
                kernelSum += kernel.W(r);
            }

            // kernelSum is never zero in practice: the particle's own
            // self-contribution W(0) is always present because
            // getNeighbors() includes the query particle. The guard covers
            // a degenerate kernel only.
            pi.volume = kernelSum > 0.0f ? 1.0f / kernelSum : 0.0f;
        }
    }
}

} // namespace aquasph
