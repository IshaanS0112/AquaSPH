#pragma once
#include <vector>
#include <functional>
#include <glm/glm.hpp>
#include "Particle.hpp"

namespace aquasph {

// What happens at a domain face. Obstacles and tank walls are boundary
// *particles* (see BoundaryVolume.hpp); this is the outer box that defines
// where the simulation exists at all.
enum class FaceMode {
    // Containment: the particle is placed back on the face and its
    // outward-normal velocity component is reversed and scaled by
    // `damping`. NOT a physics model -- with boundary particles enabled
    // this should essentially never fire, and Simulation counts how often
    // it does precisely so "essentially never" is a measurement rather
    // than a hope.
    Solid,
    // Open: the particle has left the simulated region. The integrator
    // leaves it where it is; Simulation removes it after the step. This is
    // what makes floods, rivers and channels meaningful -- without an
    // outflow, any open-domain scenario just fills up.
    Open,
    // Wrap-around, for periodic channels and infinite wave flumes.
    Periodic,
};

struct BoundaryBox {
    glm::vec3 min;
    glm::vec3 max;
    // Velocity retained (and reversed) on a containment hit.
    // 0 = inelastic/absorbing, 1 = perfectly elastic.
    float damping = 0.5f;
    // Per-face behaviour: -x, +x, -y, +y, -z, +z. Defaults keep the
    // original all-solid box, so existing aggregate initialisation
    // `BoundaryBox{min, max, damping}` behaves exactly as before.
    FaceMode faces[6] = {FaceMode::Solid, FaceMode::Solid, FaceMode::Solid,
                          FaceMode::Solid, FaceMode::Solid, FaceMode::Solid};
};

// 2nd-order predictor-corrector integrator:
//   1) predict v_half = v + (F/m) * dt/2
//   2) recompute forces using v_half (captures the velocity-dependent
//      viscosity term; pressure/gravity are unaffected since position
//      hasn't moved yet at this stage)
//   3) correct v_new = v + (F_half/m) * dt
//   4) advance position with (v_new + XSPH correction), then apply the
//      domain face conditions
//
// The force-recompute step is injected as a callback rather than the
// integrator depending on ForceCompute/LinkedCell directly -- keeps this
// class testable in isolation (see tests/test_integrator.cpp, which uses
// a trivial gravity-only callback with zero neighbors).
//
// THE VELOCITY CLAMP IS GONE. v1 capped every particle's speed at
// `max_speed` because a fixed dt could not survive the floor-impact force
// spike. Two things replaced it, in this order: the momentum equation was
// corrected (the pressure and viscous terms were being applied as though
// divided by particle mass, and the viscous term had the wrong sign, so
// it added energy instead of removing it), and dt became adaptive
// (core/TimeStep.hpp). With those in place the clamp never engages, so
// keeping it would only hide the next real instability. Measured
// before/after is in docs/architecture.md.
//
// XSPH. Position is advanced with v + Particle::xsphDelta, the Monaghan
// (1989) neighbourhood-averaged correction, while the stored velocity
// stays the momentum-carrying one. That distinction is the whole point of
// XSPH: it removes particle-scale disorder from the *advection* without
// removing momentum from the *dynamics*.
class PredictorCorrectorIntegrator {
public:
    explicit PredictorCorrectorIntegrator(const BoundaryBox& bounds);

    void step(std::vector<Particle>& particles, float dt,
              const std::function<void(std::vector<Particle>&)>& recomputeForces);

    // Number of times a Solid face had to push a particle back in-bounds
    // since the last resetContainmentEvents(). Should be ~0 in a healthy
    // run with boundary particles; a rising count means the boundary
    // sampling is too coarse for the flow speed, not that the fluid is
    // "fine because nothing escaped".
    long long containmentEvents() const { return containmentEvents_; }
    void resetContainmentEvents() { containmentEvents_ = 0; }

    // True once a particle has crossed an Open face and is waiting to be
    // removed by the owner. Cleared by resetOutflow().
    bool hasOutflow() const { return outflow_; }
    void resetOutflow() { outflow_ = false; }

    const BoundaryBox& bounds() const { return bounds_; }

private:
    BoundaryBox bounds_;
    long long containmentEvents_ = 0;
    bool outflow_ = false;

    void applyBoundary(Particle& p, long long& events, bool& outflow) const;
};

} // namespace aquasph
