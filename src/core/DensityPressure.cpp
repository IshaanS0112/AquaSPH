#include "DensityPressure.hpp"
#include "../spatial/LinkedCell.hpp"

namespace aquasph {

// PARALLELIZED OVER PARTICLES. Each iteration writes only
// particles[i].density/pressure -- unique per i, so no thread ever
// writes memory another thread reads or writes -- and every read
// (particles[j].position/mass, via the const LinkedCell/kernel/eos) is
// unmodified during this pass. That makes this embarrassingly parallel
// with zero synchronization needed, *except* for one thing that the serial version
// got away with only because it was single-threaded: `neighbors` was a
// single vector declared once outside the loop and reused via
// .clear() every iteration. Under OpenMP that's a live data race the
// moment two threads call getNeighbors() concurrently on it. The fix is
// to give each *thread* (not each iteration) its own buffer: declared
// inside the `#pragma omp parallel` region but outside the `#pragma omp
// for`, so it's allocated once per thread and reused across that
// thread's share of iterations -- same allocation-avoidance benefit as
// the serial version had, just scoped correctly for multiple threads.
void computeDensityPressure(std::vector<Particle>& particles,
                             const LinkedCell& grid,
                             const CubicSplineKernel& kernel,
                             const TaitEOS& eos) {
    const float h = kernel.h();
    const int n = static_cast<int>(particles.size());

    #pragma omp parallel
    {
        std::vector<int> neighbors;
        neighbors.reserve(128);

        #pragma omp for schedule(static)
        for (int i = 0; i < n; ++i) {
            neighbors.clear();
            grid.getNeighbors(i, particles, neighbors);

            float density = 0.0f;
            const glm::vec3& pi = particles[i].position;

            for (int j : neighbors) {
                const glm::vec3 rij = pi - particles[j].position;
                const float r = glm::length(rij);
                if (r >= h) continue;
                density += particles[j].mass * kernel.W(r);
            }

            particles[i].density = density;
            particles[i].pressure = eos.pressure(density);
        }
    }
}

} // namespace aquasph
