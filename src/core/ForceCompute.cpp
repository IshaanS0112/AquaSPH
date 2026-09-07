#include "ForceCompute.hpp"
#include "Constants.hpp"
#include "../spatial/LinkedCell.hpp"
#include <algorithm>
#include <cmath>

namespace aquasph {

namespace {

// Akinci et al. (2013) cohesion spline, eq. 2. Deliberately NOT the SPH
// smoothing kernel: it is designed to be zero at r = 0 as well as at
// r = h, so two coincident particles feel no cohesion and clumping is not
// self-reinforcing. Using W here instead -- an easy substitution to make
// -- produces exactly the particle clustering surface tension is supposed
// to prevent.
inline float cohesionSpline(float r, float h) {
    if (r <= 0.0f || r >= h) return 0.0f;
    const float h3 = h * h * h;
    const float h9 = h3 * h3 * h3;
    const float sigma = 32.0f / (constants::kPi * h9);
    const float d = h - r;
    const float d3 = d * d * d;
    const float r3 = r * r * r;
    if (2.0f * r > h) {
        return sigma * d3 * r3;
    }
    const float h6 = h3 * h3;
    return sigma * (2.0f * d3 * r3 - h6 / 64.0f);
}

} // namespace

void computeSurfaceNormals(std::vector<Particle>& particles,
                            const LinkedCell& grid,
                            const CubicSplineKernel& kernel,
                            const MaterialTable& materials) {
    bool anyTension = false;
    for (const Material& m : materials) {
        if (m.surfaceTension > 0.0f) { anyTension = true; break; }
    }
    if (!anyTension) return;

    const float h = kernel.h();
    const int n = static_cast<int>(particles.size());

    #pragma omp parallel
    {
        std::vector<int> neighbors;
        neighbors.reserve(128);

        #pragma omp for schedule(static)
        for (int i = 0; i < n; ++i) {
            Particle& pi = particles[i];
            if (pi.kind != ParticleKind::Fluid) continue;

            neighbors.clear();
            grid.getNeighbors(i, particles, neighbors);

            glm::vec3 nrm(0.0f);
            for (int j : neighbors) {
                if (j == i) continue;
                const Particle& pj = particles[j];
                // Boundary particles are excluded on purpose: including
                // them would make the wall look like fluid to the colour
                // field, so a flat pool would grow a spurious "surface"
                // along the floor and be pulled off it.
                if (pj.kind != ParticleKind::Fluid || pj.density <= 0.0f) continue;
                const glm::vec3 rij = pi.position - pj.position;
                const float r = glm::length(rij);
                if (r >= h || r < constants::kEpsilon) continue;
                nrm += (pj.mass / pj.density) * kernel.gradW(rij);
            }
            pi.normal = h * nrm;
        }
    }
}

// PARALLELIZED OVER PARTICLES -- same argument as
// computeDensityPressure: each iteration writes only particles[i].force,
// .xsphDelta (unique per i), reads everything else without mutating it,
// so it is safe with zero synchronization once `neighbors` is made a
// per-thread buffer (declared inside the `#pragma omp parallel` region)
// instead of the single shared-and-reused vector the serial version used.
void computeForces(std::vector<Particle>& particles,
                    const LinkedCell& grid,
                    const CubicSplineKernel& kernel,
                    const ForceParams& params) {
    const float h = kernel.h();
    const float epsH2 = 0.01f * h * h;
    const int n = static_cast<int>(particles.size());
    const MaterialTable* mats = params.materials;
    const bool useXsph = params.xsphEpsilon > 0.0f;

    #pragma omp parallel
    {
        std::vector<int> neighbors;
        neighbors.reserve(128);

        #pragma omp for schedule(static)
        for (int i = 0; i < n; ++i) {
            Particle& pi = particles[i];
            if (pi.kind != ParticleKind::Fluid) {
                pi.force = glm::vec3(0.0f);
                pi.xsphDelta = glm::vec3(0.0f);
                continue;
            }

            const size_t mi = (mats && !mats->empty())
                                  ? std::min<size_t>(pi.material, mats->size() - 1)
                                  : 0;
            const float muI       = mats ? (*mats)[mi].viscosity       : params.viscosity;
            const float gammaST   = mats ? (*mats)[mi].surfaceTension  : 0.0f;
            const float rho0I     = mats ? (*mats)[mi].restDensity     : 0.0f;

            // ACCUMULATED AS AN ACCELERATION (m/s^2), converted to a force
            // exactly once at the bottom of the loop.
            //
            // THE BUG THIS REPLACES: the previous version summed the
            // pressure and viscous terms -- which are accelerations, as
            // the dimensional check in docs/architecture.md shows -- into
            // the same accumulator it seeded with `m_i * gravity`, a
            // force. The integrator then divided the total by m_i, so
            // gravity came out right and the SPH terms came out scaled by
            // 1/m_i: at the default scenario's m_i = 0.091 kg, an eleven-
            // fold overstatement of every pressure gradient in the
            // simulation. Keeping the accumulator in one unit for the
            // whole loop is what makes that class of error impossible
            // rather than merely fixed.
            glm::vec3 accel = params.bodyAcceleration;

            glm::vec3 xsph(0.0f);

            if (pi.density > 0.0f) {
                neighbors.clear();
                grid.getNeighbors(i, particles, neighbors);
                const float Pi_term = pi.pressure / (pi.density * pi.density);

                for (int j : neighbors) {
                    if (j == i) continue;
                    const Particle& pj = particles[j];

                    const glm::vec3 rij = pi.position - pj.position;
                    const float r = glm::length(rij);
                    if (r >= h || r < constants::kEpsilon) continue;
                    const glm::vec3 grad = kernel.gradW(rij);
                    const float rij_dot_grad = glm::dot(rij, grad);
                    const float denom = glm::dot(rij, rij) + epsH2;

                    if (pj.kind == ParticleKind::Boundary) {
                        if (!mats) continue;
                        // --- Akinci et al. (2012) rigid-fluid coupling ---
                        // The wall acts through a pseudo-mass Psi_b that is
                        // independent of how finely it was sampled (see
                        // BoundaryVolume.hpp), mirroring the fluid
                        // particle's own pressure. This is a *repulsion
                        // that grows continuously as the particle
                        // approaches*, which is why it can replace the old
                        // clamp-and-damp wall: the particle is decelerated
                        // before contact instead of being teleported back
                        // in-bounds after penetrating.
                        const float psi = rho0I * pj.volume;
                        accel += -psi * Pi_term * grad;

                        // Boundary friction: the same Morris viscous term,
                        // with the wall standing in as a fluid at rest
                        // density and at rest (or at its prescribed
                        // velocity, for a moving paddle or piston).
                        if (params.boundaryFriction > 0.0f && rho0I > 0.0f) {
                            const float muB = muI * params.boundaryFriction;
                            const float coeff = psi * (muI + muB) / (pi.density * rho0I);
                            accel += coeff * (rij_dot_grad / denom) * (pi.velocity - pj.velocity);
                        }
                        continue;
                    }

                    if (pj.density <= 0.0f) continue;

                    // --- Pressure gradient force (symmetric form) ---
                    // a_i += -m_j * (P_i/rho_i^2 + P_j/rho_j^2) * gradW_ij
                    // Symmetric in i/j by construction, so it satisfies
                    // Newton's third law pairwise (momentum-conserving).
                    const float Pj_term = pj.pressure / (pj.density * pj.density);
                    accel += -pj.mass * (Pi_term + Pj_term) * grad;

                    // --- Viscosity (Morris, Fox & Zhu 1997) ---
                    // CORRECTED FROM THE ORIGINAL SPEC in two separate
                    // ways. The spec's literal formula is
                    //   mu * sum_j m_j * (v_j - v_i) * gradW_ij / rho_j,
                    // i.e. a vector (v_j - v_i) multiplied by a vector
                    // (gradW_ij) with no dot/cross specified -- not a
                    // well-defined force, and component-wise it would not
                    // be rotationally invariant. The standard fix folds
                    // gradW into a *scalar* via r_ij . gradW_ij / |r_ij|^2.
                    //
                    // The second correction is the sign, and it was a real
                    // shipped bug rather than a spec ambiguity. r_ij .
                    // gradW_ij is NEGATIVE everywhere inside the support
                    // (the kernel decreases with r), so pairing it with
                    // (v_j - v_i) yields a term along +(v_i - v_j): the
                    // "viscosity" accelerated particles *apart* in
                    // proportion to their relative velocity, injecting
                    // energy exactly where a viscous term must remove it.
                    // Morris pairs the negative scalar with (v_i - v_j),
                    // which damps. tests/test_forces.cpp pins this down
                    // with a two-particle shear pair.
                    const float muJ = mats ? (*mats)[std::min<size_t>(pj.material, mats->size() - 1)].viscosity
                                            : params.viscosity;
                    const float visc = pj.mass * (muI + muJ) / (pi.density * pj.density);
                    accel += visc * (rij_dot_grad / denom) * (pi.velocity - pj.velocity);

                    // --- Surface tension (Akinci et al. 2013) ---
                    if (gammaST > 0.0f) {
                        // K_ij corrects the well-known artefact that a
                        // particle in a density-deficient region would
                        // otherwise be pulled harder than one in a full
                        // neighbourhood, which biases droplets towards
                        // their own free surface.
                        const float K = 2.0f * rho0I / (pi.density + pj.density);
                        const glm::vec3 cohesion =
                            -gammaST * pj.mass * cohesionSpline(r, h) * (rij / r);
                        const glm::vec3 curvature = -gammaST * (pi.normal - pj.normal);
                        accel += K * (cohesion + curvature);
                    }

                    // --- XSPH velocity correction (Monaghan 1989) ---
                    // Accumulated separately and NEVER added to the force:
                    // XSPH moves a particle with its neighbourhood-averaged
                    // velocity while leaving the momentum-carrying velocity
                    // alone. Folding it into the force instead would make it
                    // a physical drag term and change the momentum balance.
                    if (useXsph) {
                        xsph += (2.0f * pj.mass / (pi.density + pj.density))
                                * (pj.velocity - pi.velocity) * kernel.W(r);
                    }
                }
            }

            pi.force = pi.mass * accel;
            pi.xsphDelta = params.xsphEpsilon * xsph;
        }
    }
}

} // namespace aquasph
