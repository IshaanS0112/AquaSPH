#include "Scenario.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace aquasph {

const char* tierLabel(Tier t) {
    switch (t) {
        case Tier::One: return "Tier 1 (physically demonstrable)";
        case Tier::Two: return "Tier 2 (large-scale visual experiment)";
    }
    return "unknown tier";
}

const char* tierCaveat(Tier t) {
    switch (t) {
        case Tier::One:
            return "Resolves the named phenomenon at this particle count; "
                   "comparable against experiment or an analytical result.";
        case Tier::Two:
            return "Qualitative demonstration only. NOT quantitatively predictive "
                   "at this resolution and must not be presented as a forecast.";
    }
    return "";
}

glm::vec3 FluidRegion::velocityAt(const glm::vec3& p, const glm::vec3& regionCentre) const {
    switch (profile) {
        case VelocityProfile::Uniform:
            return velocity;
        case VelocityProfile::Shear: {
            const int a = std::clamp(shearAxis, 0, 2);
            return velocity + shearDir * (shearRate * (p[a] - shearOrigin));
        }
        case VelocityProfile::Vortex: {
            const int a = std::clamp(shearAxis, 0, 2);
            glm::vec3 axisVec(0.0f);
            axisVec[a] = 1.0f;
            return velocity + shearRate * glm::cross(axisVec, p - regionCentre);
        }
    }
    return velocity;
}

bool Motion::isStatic() const {
    return displacement.kind == TimeSeries::Kind::Constant && displacement.value == 0.0f;
}

glm::vec3 Motion::translationAt(float t) const {
    return axis * displacement.at(t);
}

glm::vec3 Motion::velocityAt(float t) const {
    // Central difference. Uniform across every TimeSeries kind (including
    // the piecewise-linear Keyframes case, where an analytic derivative
    // would be discontinuous at the knots), and exactly reproducible for
    // a given t -- it must be, since this velocity enters the boundary
    // friction term and therefore the physics.
    constexpr float kEps = 1.0e-4f;
    const float a = displacement.at(t + kEps);
    const float b = displacement.at(t - kEps);
    return axis * ((a - b) / (2.0f * kEps));
}

Obstacle WaveGenerator::toObstacle() const {
    Obstacle obs;
    obs.name = name;

    const int ax = std::clamp(axis, 0, 2);
    obs.shape.type = ShapeType::Box;
    glm::vec3 lo = spanMin;
    glm::vec3 hi = spanMax;
    lo[ax] = position - thickness;
    hi[ax] = position;
    obs.shape.min = lo;
    obs.shape.max = hi;

    obs.motion.axis = glm::vec3(0.0f);
    obs.motion.axis[ax] = 1.0f;

    TimeSeries& d = obs.motion.displacement;
    d.start = startTime;
    d.rampTime = rampTime;
    d.phase = phase;
    d.period = period;
    d.value = amplitude;

    switch (mode) {
        case Mode::Sinusoidal:
            d.kind = TimeSeries::Kind::Sinusoidal;
            break;
        case Mode::Damped:
            d.kind = TimeSeries::Kind::Damped;
            d.decay = decay;
            break;
        case Mode::Pulse: {
            // A single sudden displacement: the paddle advances once,
            // over `period`, and stays there. This is the standard
            // laboratory way to launch a solitary long-wave pulse, and it
            // is the honest mechanism behind the tsunami_pulse scenario --
            // a long-wave propagation experiment, not a tsunami model.
            d.kind = TimeSeries::Kind::Keyframes;
            d.keys = {
                glm::vec2(startTime, 0.0f),
                glm::vec2(startTime + std::max(period, 1.0e-3f), amplitude),
                glm::vec2(startTime + std::max(period, 1.0e-3f) + 1.0e3f, amplitude),
            };
            break;
        }
        case Mode::Superposition: {
            // Sampled to keyframes rather than given a closed form: the
            // sum of several sinusoids is not a TimeSeries kind, and
            // adding one would mean a variadic TimeSeries. Sampling at
            // 200 points per shortest period keeps the piecewise-linear
            // reconstruction well inside the paddle's own discretisation
            // error while leaving TimeSeries a simple, testable type.
            float shortest = 1.0e9f;
            for (const WaveComponent& c : components) shortest = std::min(shortest, c.period);
            if (components.empty() || shortest <= 0.0f) {
                d.kind = TimeSeries::Kind::Constant;
                d.value = 0.0f;
                break;
            }
            const float span = duration > 0.0f ? duration : 60.0f;
            const float dt = shortest / 200.0f;
            const int n = std::min(200000, static_cast<int>(span / dt) + 1);
            d.kind = TimeSeries::Kind::Keyframes;
            d.keys.reserve(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i) {
                const float t = startTime + static_cast<float>(i) * dt;
                float sum = 0.0f;
                for (const WaveComponent& c : components) {
                    const float omega = 2.0f * 3.14159265358979f / c.period;
                    sum += c.amplitude * std::sin(omega * (t - startTime) + c.phase);
                }
                float env = 1.0f;
                if (rampTime > 0.0f) {
                    const float u = std::clamp((t - startTime) / rampTime, 0.0f, 1.0f);
                    env = u * u * (3.0f - 2.0f * u);
                }
                d.keys.emplace_back(t, sum * env);
            }
            break;
        }
    }

    // A finite duration parks the paddle at its last position rather than
    // letting it keep stroking: the point of a finite wave train is to
    // watch what happens after the forcing stops.
    if (duration > 0.0f && d.kind != TimeSeries::Kind::Keyframes) {
        const float stop = startTime + duration;
        const int n = 2000;
        std::vector<glm::vec2> keys;
        keys.reserve(static_cast<size_t>(n) + 1);
        for (int i = 0; i <= n; ++i) {
            const float t = startTime + duration * static_cast<float>(i) / static_cast<float>(n);
            keys.emplace_back(t, d.at(t));
        }
        keys.emplace_back(stop + 1.0e3f, keys.back().y);
        TimeSeries parked;
        parked.kind = TimeSeries::Kind::Keyframes;
        parked.keys = std::move(keys);
        d = parked;
    }

    return obs;
}

std::uint8_t Scenario::materialIndex(const std::string& matName) const {
    for (size_t i = 0; i < materials.size(); ++i) {
        if (materials[i].name == matName) return static_cast<std::uint8_t>(i);
    }
    std::cerr << "[Scenario] Warning: material '" << matName << "' not defined in scenario '"
              << name << "'; falling back to '"
              << (materials.empty() ? std::string("<none>") : materials[0].name) << "'.\n";
    return 0;
}

} // namespace aquasph
