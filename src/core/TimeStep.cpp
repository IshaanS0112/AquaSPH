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

    // Fixed-chunk deterministic reductions, NOT `reduction(max:)` -- see
    // ParallelReduce.hpp for why the guarantee is made structural here
    // even though max over finite floats happens to be order-independent.
    // Boundary particles are excluded: a prescribed paddle velocity is not
    // a stability constraint on the fluid integration, and including it
    // would let a fast gate throttle the whole simulation.
    //
    // NaN HANDLING. std::max(x, NaN) returns x, so a NaN silently
    // disappears from a max reduction -- the controller would compute a
    // comfortable dt for a state that has already diverged, and the run
    // would coast on to a misleading verdict. Non-finite values are
    // therefore mapped to +infinity, which dominates the max
    // deterministically and drives dt to its floor, where the stability
    // check can see the state for what it is.
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
        // Reached only when the state already contains NaN/Inf. Fall back
        // to dt_min so the run continues to its stability verdict and
        // reports UNSTABLE, rather than propagating a NaN dt and turning
        // every subsequent diagnostic into NaN too.
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
