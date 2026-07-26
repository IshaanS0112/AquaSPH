#pragma once
#include <vector>
#include <glm/glm.hpp>
#include "Particle.hpp"
#include "SPHKernel.hpp"

namespace aquasph {

class LinkedCell;

struct ForceParams {
    float viscosity;    // mu: tunable artificial viscous damping coefficient
    glm::vec3 gravity;  // e.g. (0, -9.81, 0)
};

// Resets each particle's force to m*gravity, then accumulates the
// symmetric SPH pressure-gradient force and a viscous diffusion force
// from its neighbors (see ForceCompute.cpp for why the viscosity term
// differs from the original spec's literal formula).
void computeForces(std::vector<Particle>& particles,
                    const LinkedCell& grid,
                    const CubicSplineKernel& kernel,
                    const ForceParams& params);

} // namespace aquasph
