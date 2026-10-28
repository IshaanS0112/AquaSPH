#pragma once
#include <vector>
#include <glm/glm.hpp>

namespace aquasph {

// One scalar function of time, f(t), reused everywhere a scenario needs something to change
// during the run: emitter flow rate, external force magnitude, wave-paddle displacement, gate
// opening.
struct TimeSeries {
    enum class Kind {
        Constant,     // value
        Ramp,         // 0 -> value linearly over [start, end], then value
        Pulse,        // value on [start, start+duration], 0 otherwise
        Sinusoidal,   // offset + value * sin(2*pi*t/period + phase)
        Damped,       // Sinusoidal * exp(-decay * (t - start))
        Keyframes,    // piecewise-linear through (t, v) pairs; the "scripted" case
    };

    Kind kind = Kind::Constant;
    float value = 1.0f;      // constant level, or sinusoid amplitude
    float offset = 0.0f;     // sinusoid mean
    float start = 0.0f;
    float end = 0.0f;        // Ramp only
    float duration = 0.0f;   // Pulse only
    float period = 1.0f;     // Sinusoidal / Damped
    float phase = 0.0f;      // radians
    float decay = 0.0f;      // Damped, 1/s
    // Envelope applied to Sinusoidal/Damped so a wave train starts from rest instead of
    // stepping the paddle to full stroke on step 0.
    float rampTime = 0.0f;
    std::vector<glm::vec2> keys;   // (t, value), assumed sorted by t

    float at(float t) const;

    // The identically-zero series. Named rather than written as an aggregate initialiser at
    // each use site, because a partial aggregate initialiser is a -Wmissing-field-initializers
    // warning and a silent trap the day a member is added.
    static TimeSeries zero() {
        TimeSeries ts;
        ts.kind = Kind::Constant;
        ts.value = 0.0f;
        return ts;
    }
};

} // namespace aquasph
