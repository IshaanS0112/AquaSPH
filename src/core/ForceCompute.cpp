#include "ForceCompute.hpp"
#include "Constants.hpp"
#include "../spatial/LinkedCell.hpp"

namespace aquasph {

// PARALLELIZED OVER PARTICLES -- same argument as
// computeDensityPressure: each iteration writes only particles[i].force
// (unique per i), reads everything else without mutating it, so it's
// safe with zero synchronization once `neighbors` is made a per-thread
// buffer (declared inside the `#pragma omp parallel` region) instead of
// the single shared-and-reused vector the serial version used.
void computeForces(std::vector<Particle>& particles,
                    const LinkedCell& grid,
                    const CubicSplineKernel& kernel,
                    const ForceParams& params) {
    const float h = kernel.h();
    const float epsH2 = 0.01f * h * h;
    const int n = static_cast<int>(particles.size());

    #pragma omp parallel
    {
        std::vector<int> neighbors;
        neighbors.reserve(128);

        #pragma omp for schedule(static)
        for (int i = 0; i < n; ++i) {
            Particle& pi = particles[i];
            glm::vec3 force = pi.mass * params.gravity;

            if (pi.density > 0.0f) {
                neighbors.clear();
                grid.getNeighbors(i, particles, neighbors);
                const float Pi_term = pi.pressure / (pi.density * pi.density);

                for (int j : neighbors) {
                    if (j == i) continue;
                    const Particle& pj = particles[j];
                    if (pj.density <= 0.0f) continue;

                    const glm::vec3 rij = pi.position - pj.position;
                    const float r = glm::length(rij);
                    if (r >= h || r < constants::kEpsilon) continue;

                    const glm::vec3 grad = kernel.gradW(rij);

                    // --- Pressure gradient force (symmetric form) ---
                    // F_i += -m_j * (P_i/rho_i^2 + P_j/rho_j^2) * gradW_ij
                    // Symmetric in i/j by construction, so it satisfies
                    // Newton's third law pairwise (momentum-conserving).
                    const float Pj_term = pj.pressure / (pj.density * pj.density);
                    force += -pj.mass * (Pi_term + Pj_term) * grad;

                    // --- Viscosity force ---
                    // CORRECTED FROM THE ORIGINAL SPEC: the spec's literal
                    // formula is  mu * sum_j m_j * (v_j - v_i) * gradW_ij / rho_j,
                    // i.e. a vector (v_j - v_i) multiplied by a vector
                    // (gradW_ij). That's not a well-defined operation for
                    // producing a force (no dot/cross specified), and taken
                    // component-wise it would not be rotationally invariant
                    // (the simulation's behavior would depend on how the
                    // domain axes happen to be oriented -- physically wrong).
                    //
                    // Instead we use the standard SPH discretization of the
                    // velocity Laplacian (Monaghan; Morris et al. 1997),
                    // which folds gradW into a *scalar* via
                    // dot(r_ij, gradW_ij) / |r_ij|^2. This reuses the same
                    // cubic-spline kernel gradient (no second kernel needed),
                    // is rotationally invariant, and reduces to the spec's
                    // intent (mu, mass, velocity difference, density,
                    // kernel gradient all still appear) while being
                    // dimensionally and physically sound. `mu` remains a
                    // tunable damping coefficient rather than a literal SI
                    // viscosity, exactly as the project notes acknowledge
                    // ("Viscosity is artificial").
                    const float rij_dot_grad = glm::dot(rij, grad);
                    const float denom = glm::dot(rij, rij) + epsH2;
                    force += params.viscosity * pj.mass * (pj.velocity - pi.velocity)
                             * (2.0f * rij_dot_grad / denom) / pj.density;
                }
            }

            pi.force = force;
        }
    }
}

} // namespace aquasph
