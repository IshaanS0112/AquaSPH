#pragma once
#include <vector>
#include "Particle.hpp"
#include "SPHKernel.hpp"

namespace aquasph {

class LinkedCell;

// Akinci et al., "Versatile Rigid-Fluid Coupling for Incompressible SPH" (SIGGRAPH 2012),
// section 3.
void computeBoundaryVolumes(std::vector<Particle>& particles,
                             const LinkedCell& grid,
                             const CubicSplineKernel& kernel,
                             int boundaryEnd = -1);

} // namespace aquasph
