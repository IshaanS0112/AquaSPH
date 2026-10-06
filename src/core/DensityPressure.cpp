#include "DensityPressure.hpp"
#include "../spatial/LinkedCell.hpp"

namespace aquasph {

// PARALLELIZED OVER PARTICLES. Each iteration writes only particles[i].density/pressure.
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


// Boundary/material-aware variant. Same parallelisation argument as above (each iteration
// writes only particles[i]).
void computeDensityPressure(std::vector<Particle>& particles,
                             const LinkedCell& grid,
                             const CubicSplineKernel& kernel,
                             const std::vector<TaitEOS>& eosByMaterial,
                             const std::vector<float>& restDensityByMaterial,
                             int firstFluid) {
    const float h = kernel.h();
    const int n = static_cast<int>(particles.size());
    if (eosByMaterial.empty()) return;
    const int begin = std::max(0, firstFluid);

    #pragma omp parallel
    {
        std::vector<int> neighbors;
        neighbors.reserve(128);

        #pragma omp for schedule(static)
        for (int i = begin; i < n; ++i) {
            Particle& pi = particles[i];
            if (pi.kind != ParticleKind::Fluid) continue;

            const size_t mi = std::min<size_t>(pi.material, eosByMaterial.size() - 1);
            const float rho0i = restDensityByMaterial[mi];

            neighbors.clear();
            grid.getNeighbors(i, particles, neighbors);

            float density = 0.0f;
            const glm::vec3& xi = pi.position;

            for (int j : neighbors) {
                const Particle& pj = particles[j];
                const float r = glm::length(xi - pj.position);
                if (r >= h) continue;
                // Fluid neighbours contribute their real mass.
                const float w = (pj.kind == ParticleKind::Fluid) ? pj.mass : rho0i * pj.volume;
                density += w * kernel.W(r);
            }

            pi.density = density;
            pi.pressure = eosByMaterial[mi].pressure(density);
        }
    }
}

} // namespace aquasph
