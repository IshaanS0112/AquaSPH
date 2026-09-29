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
    glm::vec3 bodyAcceleration{0.0f, -9.81f, 0.0f};

    // Dynamic viscosity used when `materials` is null (single-material
    // legacy path). Pa*s, in the Morris et al. (1997) discretisation.
    float viscosity = 5.0f;

    // Monaghan (1989) XSPH coefficient. 0 disables the correction and
    // skips its accumulation entirely.
    float xsphEpsilon = 0.0f;

    // How much of a material's viscosity acts against boundary particles. 0 = free slip (the
    // wall only resists penetration), 1 = full no-slip.
    float boundaryFriction = 1.0f;

    // Per-particle material lookup. Null => every particle uses
    // `viscosity` above, no surface tension, and no boundary coupling.
    const MaterialTable* materials = nullptr;

    // Index of the first fluid particle.
    int firstFluidIndex = 0;
};

// Accumulates each fluid particle's force: pressure, Morris viscosity, Akinci surface tension
// and boundary terms. The XSPH correction goes into xsphDelta, not the force.
void computeForces(std::vector<Particle>& particles,
                    const LinkedCell& grid,
                    const CubicSplineKernel& kernel,
                    const ForceParams& params);

// Colour-field normals for the surface-tension curvature term: A separate pass because the
// curvature term needs *every* neighbour's normal, which cannot be known while the force loop
// is still computing them.
void computeSurfaceNormals(std::vector<Particle>& particles,
                            const LinkedCell& grid,
                            const CubicSplineKernel& kernel,
                            const MaterialTable& materials,
                            int firstFluid = 0);

} // namespace aquasph
