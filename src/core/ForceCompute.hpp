#pragma once
#include <vector>
#include <glm/glm.hpp>
#include "Particle.hpp"
#include "SPHKernel.hpp"
#include "Material.hpp"

namespace aquasph {

class LinkedCell;

struct ForceParams {
    // Uniform body acceleration applied to every fluid particle, m/s^2.
    // Gravity plus whatever the scenario's ExternalForce list evaluates to
    // at the current time -- the solver deliberately does not know which
    // is which. Sloshing tanks, shaken platforms and pulse forcing are all
    // just a different a(t) arriving here.
    glm::vec3 bodyAcceleration{0.0f, -9.81f, 0.0f};

    // Dynamic viscosity used when `materials` is null (single-material
    // legacy path). Pa*s, in the Morris et al. (1997) discretisation.
    float viscosity = 5.0f;

    // Monaghan (1989) XSPH coefficient. 0 disables the correction and
    // skips its accumulation entirely.
    float xsphEpsilon = 0.0f;

    // How much of a material's viscosity acts against boundary particles.
    // 0 = free slip (the wall only resists penetration), 1 = full no-slip.
    // Real tank walls are no-slip; the free-slip end exists because wall
    // friction is one of the named reasons SPH dam breaks disagree with
    // Martin & Moyce, and being able to turn it off makes that testable
    // rather than assumed.
    float boundaryFriction = 1.0f;

    // Per-particle material lookup. Null => every particle uses
    // `viscosity` above, no surface tension, and no boundary coupling.
    const MaterialTable* materials = nullptr;
};

// Resets each fluid particle's force and accumulates, from its neighbours:
// the symmetric SPH pressure gradient, the Morris viscous term, the Akinci
// et al. (2013) surface-tension cohesion + curvature terms, the Akinci et
// al. (2012) boundary pressure and friction terms, and (into
// Particle::xsphDelta, not into the force) the Monaghan XSPH correction.
//
// Boundary particles are skipped as `i` -- they are never integrated.
void computeForces(std::vector<Particle>& particles,
                    const LinkedCell& grid,
                    const CubicSplineKernel& kernel,
                    const ForceParams& params);

// Colour-field normals for the surface-tension curvature term:
//     n_i = h * sum_j (m_j / rho_j) * gradW_ij     (fluid neighbours only)
//
// A separate pass because the curvature term needs *every* neighbour's
// normal, which cannot be known while the force loop is still computing
// them. Depends only on positions and densities, so it is computed once
// per step before computeForces and stays valid across the integrator's
// mid-step force re-evaluation (which does not move particles).
//
// No-ops when no material in the table has a non-zero surface tension,
// so scenarios that do not need it pay nothing.
void computeSurfaceNormals(std::vector<Particle>& particles,
                            const LinkedCell& grid,
                            const CubicSplineKernel& kernel,
                            const MaterialTable& materials);

} // namespace aquasph
