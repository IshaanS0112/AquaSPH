#include "TimeSeries.hpp"
#include "../core/Constants.hpp"
#include <algorithm>
#include <cmath>

namespace aquasph {

namespace {
float rampEnvelope(float t, float start, float rampTime) {
    if (rampTime <= 0.0f) return 1.0f;
    const float u = (t - start) / rampTime;
    if (u <= 0.0f) return 0.0f;
    if (u >= 1.0f) return 1.0f;
    // Smoothstep rather than linear: a linear ramp has a discontinuous second derivative at
    // both ends, which a wave paddle turns into a small but measurable transient.
    return u * u * (3.0f - 2.0f * u);
}
} // namespace

float TimeSeries::at(float t) const {
    switch (kind) {
        case Kind::Constant:
            return value;

        case Kind::Ramp: {
            if (t <= start) return 0.0f;
            if (end <= start || t >= end) return value;
            return value * (t - start) / (end - start);
        }

        case Kind::Pulse:
            return (t >= start && t <= start + duration) ? value : 0.0f;

        case Kind::Sinusoidal: {
            if (t < start) return offset;
            const float omega = 2.0f * constants::kPi / (period > 0.0f ? period : 1.0f);
            return offset + value * rampEnvelope(t, start, rampTime)
                             * std::sin(omega * (t - start) + phase);
        }

        case Kind::Damped: {
            if (t < start) return offset;
            const float omega = 2.0f * constants::kPi / (period > 0.0f ? period : 1.0f);
            const float env = std::exp(-decay * (t - start)) * rampEnvelope(t, start, rampTime);
            return offset + value * env * std::sin(omega * (t - start) + phase);
        }

        case Kind::Keyframes: {
            if (keys.empty()) return value;
            if (t <= keys.front().x) return keys.front().y;
            if (t >= keys.back().x) return keys.back().y;
            for (size_t i = 1; i < keys.size(); ++i) {
                if (t <= keys[i].x) {
                    const float span = keys[i].x - keys[i - 1].x;
                    if (span <= 0.0f) return keys[i].y;
                    const float u = (t - keys[i - 1].x) / span;
                    return keys[i - 1].y + u * (keys[i].y - keys[i - 1].y);
                }
            }
            return keys.back().y;
        }
    }
    return value;
}

} // namespace aquasph
