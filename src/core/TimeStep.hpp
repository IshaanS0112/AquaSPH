#pragma once
#include <vector>
#include "Particle.hpp"

namespace aquasph {

// Adaptive timestep control. WHY THIS EXISTS.
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

    // True when `dt` differs from `previous` by more than one order of magnitude.
    static bool isOrderOfMagnitudeChange(float previous, float dt);

    const TimeStepParams& params() const { return params_; }

private:
    TimeStepParams params_;
    float h_;
    float maxSoundSpeed_;
    float maxKinematicViscosity_;
};

} // namespace aquasph
