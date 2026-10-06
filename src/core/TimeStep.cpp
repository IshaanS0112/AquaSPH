#include "TimeStep.hpp"
#include "ParallelReduce.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace aquasph {

TimeStepController::TimeStepController(const TimeStepParams& params, float h,
                                        float maxSoundSpeed, float maxKinematicViscosity)
    : params_(params), h_(h), maxSoundSpeed_(maxSoundSpeed),
      maxKinematicViscosity_(maxKinematicViscosity) {}

TimeStepInfo TimeStepController::compute(const std::vector<Particle>& particles) const {
    const int n = static_cast<int>(particles.size());

    // Fixed-chunk deterministic reductions, NOT `reduction(max:)`.
    const auto finiteOrInf = [](float v) {
        return std::isfinite(v) ? v : std::numeric_limits<float>::infinity();
    };

    const float maxSpeed = reduce::deterministicMax(n, 0.0f, [&](int i) {
        return particles[i].kind == ParticleKind::Fluid
                   ? finiteOrInf(glm::length(particles[i].velocity))
                   : 0.0f;
    });

    const float maxAccel = reduce::deterministicMax(n, 0.0f, [&](int i) {
        const Particle& p = particles[i];
        if (p.kind != ParticleKind::Fluid || p.mass <= 0.0f) return 0.0f;
        return finiteOrInf(glm::length(p.force) / p.mass);
    });

    TimeStepInfo info;
    info.maxSpeed = maxSpeed;
    info.maxAccel = maxAccel;

    const float big = std::numeric_limits<float>::max();

    info.dtCfl = params_.cflCoeff * h_ / (maxSoundSpeed_ + maxSpeed);

    info.dtForce = (maxAccel > 0.0f)
                        ? params_.forceCoeff * std::sqrt(h_ / maxAccel)
                        : big;

    info.dtViscous = (maxKinematicViscosity_ > 0.0f)
                          ? params_.viscousCoeff * h_ * h_ / maxKinematicViscosity_
                          : big;

    float dt = std::min({info.dtCfl, info.dtForce, info.dtViscous});

    if (!std::isfinite(dt)) {
        // Reached only when the state already contains NaN/Inf.
        dt = params_.dtMin;
        info.clampedToMin = true;
    }
    if (dt < params_.dtMin) { dt = params_.dtMin; info.clampedToMin = true; }
    if (dt > params_.dtMax) { dt = params_.dtMax; info.clampedToMax = true; }

    info.dt = dt;
    return info;
}

bool TimeStepController::isOrderOfMagnitudeChange(float previous, float dt) {
    if (previous <= 0.0f || dt <= 0.0f) return false;
    const float ratio = dt / previous;
    return ratio >= 10.0f || ratio <= 0.1f;
}

} // namespace aquasph
