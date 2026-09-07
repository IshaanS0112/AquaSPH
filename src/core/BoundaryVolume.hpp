#pragma once
#include <vector>
#include "Particle.hpp"
#include "SPHKernel.hpp"

namespace aquasph {

class LinkedCell;

// Akinci et al., "Versatile Rigid-Fluid Coupling for Incompressible SPH"
// (SIGGRAPH 2012), section 3.
//
// Boundary geometry is represented by static particles sampled over the
// solid's surface. The problem this solves: a naive "boundary particles
// have mass m too" scheme makes the wall's strength depend on how densely
// it was sampled -- sample the floor twice as finely and the fluid gets
// pushed twice as hard. Akinci's fix is to give each boundary particle a
// *volume*
//
//     V_b = 1 / sum_k W(|r_b - r_k|, h)     (k over boundary particles only)
//
// and let it act on fluid particle i with the pseudo-mass
// Psi_b = rho0_i * V_b. Where boundary particles are packed densely the
// kernel sum is large and each one's volume is correspondingly small, so
// the total boundary contribution converges to the same value regardless
// of sampling density. That is what makes arbitrary static geometry --
// boxes, spheres, cylinders, a terrain heightfield -- work through the
// same neighbour machinery with no per-shape code in the solver.
//
// Computed once after the boundary particles are placed, since static
// geometry does not move. The `position(t)` hook on Obstacle
// (scene/Obstacle.hpp) is the designed extension point for moving
// boundaries; if one is ever driven, this must be recomputed whenever the
// boundary sampling changes shape (rigid translation and rotation do not
// change it, which is why paddles and gates are the cheap first case).
void computeBoundaryVolumes(std::vector<Particle>& particles,
                             const LinkedCell& grid,
                             const CubicSplineKernel& kernel);

} // namespace aquasph
