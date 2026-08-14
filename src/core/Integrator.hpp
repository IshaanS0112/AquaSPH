#pragma once
#include <vector>
#include <functional>
#include <glm/glm.hpp>
#include "Particle.hpp"

namespace aquasph {

struct BoundaryBox {
    glm::vec3 min;
    glm::vec3 max;
    float damping = 0.5f; // velocity retained after a wall reflection (0=inelastic, 1=perfectly elastic)
};

// 2nd-order predictor-corrector integrator:
//   1) predict v_half = v + (F/m) * dt/2
//   2) recompute forces using v_half (captures the velocity-dependent
//      viscosity term; pressure/gravity are unaffected since position
//      hasn't moved yet at this stage)
//   3) correct v_new = v + (F_half/m) * dt
//   4) advance position with v_new, then apply boundary conditions
//
// The force-recompute step is injected as a callback rather than the
// integrator depending on ForceCompute/LinkedCell directly -- keeps this
// class testable in isolation (see tests/test_integrator.cpp, which uses
// a trivial gravity-only callback with zero neighbors).
//
// VELOCITY CLAMP -- WHY IT'S HERE: found necessary by actually running
// the dam-break scenario (see docs/architecture.md). Weakly-compressible
// SPH's Tait pressure grows as (rho/rho0)^gamma, so the pressure-gradient
// force's P/rho^2 term grows roughly as rho^(gamma-2) -- i.e. very
// steeply once density overshoots. When the dam-break block's bottom
// face reaches the floor, many particles hit within the same few
// timesteps and compact briefly; the resulting force spike, combined
// with a *fixed* dt (adaptive stepping is explicitly deferred to V2 by
// this spec), was enough to take a particle from 0.6 m/s to 6,500 m/s to
// literal floating-point infinity within 5 simulation steps -- classic
// explicit-integration blowup once a locally stiff force outruns a fixed
// timestep's stability limit. maxSpeed is a numerical safety valve, not
// a physical parameter: real dam-break flows stay well under 10 m/s, so
// clamping at a much higher ceiling only ever intervenes during genuine
// numerical pathology, never during normal flow.
class PredictorCorrectorIntegrator {
public:
    PredictorCorrectorIntegrator(const BoundaryBox& bounds, float maxSpeed);

    void step(std::vector<Particle>& particles, float dt,
              const std::function<void(std::vector<Particle>&)>& recomputeForces);

private:
    BoundaryBox bounds_;
    float maxSpeed_;
    void applyBoundary(Particle& p) const;
};

} // namespace aquasph
