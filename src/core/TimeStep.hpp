#pragma once
#include <vector>
#include "Particle.hpp"

namespace aquasph {

// Adaptive timestep control.
//
// WHY THIS EXISTS. With a fixed dt the solver had no way to respond to a
// transient force spike, so the only remaining lever was a hard ceiling on
// particle speed. docs/architecture.md recorded honestly that the ceiling
// then engaged continuously rather than as a rare safety net -- and the
// v1 baseline run shows it pinned at exactly max_speed for all 1000 steps
// of the default dam break. A clamp that is always active is not a safety
// net, it is an undocumented change to the equations of motion, and it
// destroys precisely the features this project now wants to show: droplet
// crowns, wave trains, jets, separation behind an obstacle.
//
// The standard weakly-compressible stability triple, all evaluated each
// step against the current state:
//
//   dt_cfl     = lambda_c * h / (c0 + |v|_max)     acoustic + advective CFL
//   dt_force   = lambda_f * sqrt(h / |a|_max)      no particle crosses a
//                                                   support radius under
//                                                   the current force
//   dt_viscous = lambda_v * h^2 / nu               viscous diffusion limit
//   dt         = clamp(min(...), dt_min, dt_max)
//
// Typical coefficients are lambda_c = 0.25, lambda_f = 0.25,
// lambda_v = 0.125 (Monaghan 1992; Morris et al. 1997).
//
// DETERMINISM. dt is a global reduction that feeds straight back into the
// integration, so a thread-count-dependent dt would make the entire
// simulation thread-count-dependent -- silently, and in a way no
// per-particle test would catch. |v|_max and |a|_max are therefore taken
// with core/ParallelReduce.hpp's fixed-chunk reductions, whose arithmetic
// does not depend on how many threads happen to run them.
struct TimeStepParams {
    float cflCoeff = 0.25f;      // lambda_c
    float forceCoeff = 0.25f;    // lambda_f
    float viscousCoeff = 0.125f; // lambda_v
    float dtMin = 1.0e-6f;
    float dtMax = 1.0e-3f;
};

struct TimeStepInfo {
    float dt = 0.0f;
    float dtCfl = 0.0f;
    float dtForce = 0.0f;
    float dtViscous = 0.0f;
    float maxSpeed = 0.0f;
    float maxAccel = 0.0f;
    bool clampedToMin = false;   // the stability triple asked for less than dt_min
    bool clampedToMax = false;
};

class TimeStepController {
public:
    TimeStepController(const TimeStepParams& params, float h,
                        float maxSoundSpeed, float maxKinematicViscosity);

    // Evaluates the triple against the current particle state. Reads
    // velocity and force; does not modify anything.
    TimeStepInfo compute(const std::vector<Particle>& particles) const;

    // True when `dt` differs from `previous` by more than one order of
    // magnitude -- worth logging, because a sudden order-of-magnitude drop
    // marks a real physical event (impact, wave breaking, jet formation)
    // rather than a numerical accident.
    static bool isOrderOfMagnitudeChange(float previous, float dt);

    const TimeStepParams& params() const { return params_; }

private:
    TimeStepParams params_;
    float h_;
    float maxSoundSpeed_;
    float maxKinematicViscosity_;
};

} // namespace aquasph
