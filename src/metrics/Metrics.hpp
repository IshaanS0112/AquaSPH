#pragma once
#include <string>
#include <vector>
#include "../scene/Simulation.hpp"

namespace aquasph {

// Machine-readable measurement, which is what separates a scenario that
// is an EXPERIMENT from one that is an animation. Every scenario emits
// these; CI asserts on them; docs/validation.md compares them against
// experiment and analytical results.
//
// Nothing here feeds back into the simulation. Measurement is strictly
// downstream of physics.

struct ProbeSeries {
    std::string name;
    glm::vec3 position{0.0f};
    std::vector<float> time;
    std::vector<float> elevation;   // free-surface y, absolute, metres
    std::vector<float> depth;       // elevation - domain floor y
};

// Results of fitting a wave train to a probe's elevation record.
struct WaveMeasurement {
    bool valid = false;
    float meanLevel = 0.0f;
    float amplitude = 0.0f;      // mean half crest-to-trough
    float period = 0.0f;         // mean zero-up-crossing interval, s
    int wavesCounted = 0;
    float steepness = 0.0f;      // H / L, using the measured L
    float wavelength = 0.0f;     // from linear dispersion at the measured period
    float celerity = 0.0f;       // wavelength / period
};

// Linear (Biesel) piston-wavemaker prediction, so commanded and measured
// can be printed side by side.
struct WavePrediction {
    bool valid = false;
    float depth = 0.0f;
    float period = 0.0f;
    float wavenumber = 0.0f;
    float wavelength = 0.0f;
    float celerity = 0.0f;
    float height = 0.0f;         // H from the transfer function
    float steepness = 0.0f;
    bool linearTheoryValid = false;   // steepness comfortably below 1/20
};

// Fluid centre-of-mass track.
//
// WHY THIS EXISTS ALONGSIDE THE SURFACE PROBES. A probe measures the
// topmost particle in a column, so its resolution floor is one particle
// spacing. That is fine for a wave three or more spacings tall, and
// useless below it -- and a standing wave small enough for linear theory
// to apply is often exactly that small. Measured on the sloshing tank:
// with the excitation reduced until the wave sat inside linear theory's
// small-amplitude range, the wave amplitude fell BELOW one spacing and
// the two end-wall probes returned periods differing by 56%.
//
// The centre of mass has no such floor. It is an average over every fluid
// particle, so its signal-to-noise improves with particle count rather
// than degrading with wave height, and for a sloshing tank its horizontal
// component oscillates at exactly the mode being measured. It is the
// right instrument for this measurement; the probes are the right
// instrument for a wave flume, where the wave is tall and the question is
// local.
struct CentroidTrack {
    std::vector<float> time;
    std::vector<float> x, y, z;
};

struct SurgeSample {
    float time = 0.0f;
    float front = 0.0f;          // absolute position along the surge axis
    float z = 0.0f;              // (front - origin) / a, dimensionless
    float tStar = 0.0f;          // t * sqrt(2 g / a), Martin & Moyce scaling
};

struct MetricsReport {
    // Identity and provenance -- every recorded result carries enough to
    // be reproduced. See docs/experiments.md.
    std::string scenario;
    std::string tier;
    std::string tierCaveat;
    std::string approximation;
    std::string gitRevision;
    std::string quality;
    int threads = 1;

    int fluidParticles = 0;
    int boundaryParticles = 0;
    int peakFluidParticles = 0;
    int emittedTotal = 0;
    int removedTotal = 0;
    bool hitParticleCeiling = false;

    float smoothingRadius = 0.0f;
    float spacing = 0.0f;
    float simulatedTime = 0.0f;
    int steps = 0;
    double wallTimeSeconds = 0.0;
    double avgStepMs = 0.0;

    float dtMin = 0.0f;
    float dtMax = 0.0f;
    float dtMean = 0.0f;

    float densityMin = 0.0f;
    float densityMax = 0.0f;
    float densityAvgFinal = 0.0f;
    float fractionNearRestFinal = 0.0f;
    float fractionNearRestMean = 0.0f;

    float maxSpeed = 0.0f;
    float maxAccel = 0.0f;
    long long containmentEvents = 0;
    int unstableParticles = 0;
    bool stable = false;

    double initialFluidVolume = 0.0;
    double finalFluidVolume = 0.0;

    CentroidTrack centroid;
    // Period fitted from the horizontal centre-of-mass oscillation.
    WaveMeasurement centroidOscillation;

    std::vector<ProbeSeries> probes;
    std::vector<WaveMeasurement> waveMeasurements;   // index-aligned with probes
    std::vector<WavePrediction> wavePredictions;     // one per wave generator
    std::vector<SurgeSample> surge;

    bool inundationTracked = false;
    float inundatedAreaFraction = 0.0f;
    float maxDepth = 0.0f;

    std::string toJson() const;
    bool writeJson(const std::string& path) const;
};

class MetricsCollector {
public:
    explicit MetricsCollector(const Scenario& scenario);

    // Called once per output interval, not once per step: a 20,000-step
    // run does not need 20,000 samples of a wave probe, and sampling on a
    // fixed simulated-time grid keeps the record comparable across
    // quality presets, which take different numbers of steps to cover the
    // same simulated time.
    void sample(const Simulation& sim);

    // Accumulates per-step aggregates that a sampled record would miss
    // (peak particle count, dt extremes, emitted/removed totals).
    void observeStep(const StepStats& stats);

    void finish(const Simulation& sim, double wallSeconds, double avgStepMs,
                 const std::string& quality, int threads, const std::string& gitRevision);

    const MetricsReport& report() const { return report_; }

    // Exposed for tests: fit a wave train to an elevation record.
    static WaveMeasurement measureWaveTrain(const std::vector<float>& time,
                                             const std::vector<float>& elevation,
                                             float depth, float gravity);
    // Exposed for tests: linear piston-wavemaker theory.
    static WavePrediction predictWave(const WaveGenerator& generator, float gravity);

private:
    const Scenario* scenario_;
    MetricsReport report_;
    float nextSampleTime_ = 0.0f;
    double dtSum_ = 0.0;
    int dtCount_ = 0;
    double nearRestSum_ = 0.0;
    int nearRestCount_ = 0;
    bool haveInitialVolume_ = false;
};

} // namespace aquasph
