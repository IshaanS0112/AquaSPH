#include "Metrics.hpp"
#include "../core/Constants.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace aquasph {

namespace {

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

std::string num(double v) {
    if (!std::isfinite(v)) return "null";   // JSON has no NaN/Infinity
    std::ostringstream os;
    os << std::setprecision(9) << v;
    return os.str();
}

// Solve omega^2 = g k tanh(k d) for k, by bisection on a bracket that is
// guaranteed to contain the root: the deep-water value k0 = omega^2/g is a
// lower bound, and the shallow-water value omega/sqrt(gd) is an upper one.
// Bisection rather than Newton because it cannot diverge and needs no
// derivative -- 60 iterations is exact to float precision and costs
// nothing at the once-per-run rate this is called.
float solveDispersion(float omega, float depth, float g) {
    if (depth <= 0.0f || omega <= 0.0f || g <= 0.0f) return 0.0f;
    float lo = omega * omega / g;               // deep-water k
    float hi = omega / std::sqrt(g * depth);    // shallow-water k
    if (hi < lo) std::swap(lo, hi);
    lo *= 0.5f;
    hi *= 2.0f;
    for (int i = 0; i < 60; ++i) {
        const float mid = 0.5f * (lo + hi);
        const float f = g * mid * std::tanh(mid * depth) - omega * omega;
        if (f > 0.0f) hi = mid; else lo = mid;
    }
    return 0.5f * (lo + hi);
}

} // namespace

MetricsCollector::MetricsCollector(const Scenario& scenario) : scenario_(&scenario) {
    report_.scenario = scenario.name;
    report_.tier = tierLabel(scenario.tier);
    report_.tierCaveat = tierCaveat(scenario.tier);
    report_.approximation = scenario.approximation;
    report_.probes.reserve(scenario.metrics.probes.size());
    for (const WaveProbe& p : scenario.metrics.probes) {
        ProbeSeries s;
        s.name = p.name;
        s.position = p.position;
        report_.probes.push_back(std::move(s));
    }
}

void MetricsCollector::observeStep(const StepStats& st) {
    if (dtCount_ == 0) {
        report_.dtMin = st.dt;
        report_.dtMax = st.dt;
    } else {
        report_.dtMin = std::min(report_.dtMin, st.dt);
        report_.dtMax = std::max(report_.dtMax, st.dt);
    }
    dtSum_ += st.dt;
    ++dtCount_;

    report_.peakFluidParticles = std::max(report_.peakFluidParticles, st.fluidCount);
    report_.emittedTotal += st.emittedThisStep;
    report_.removedTotal += st.removedThisStep;
    report_.maxSpeed = std::max(report_.maxSpeed, st.maxSpeed);
    report_.maxAccel = std::max(report_.maxAccel, st.maxAccel);
    report_.densityMax = std::max(report_.densityMax, st.densityMax);
    if (st.fluidCount > 0) {
        report_.densityMin = (report_.densityMin == 0.0f)
                                  ? st.densityMin
                                  : std::min(report_.densityMin, st.densityMin);
        nearRestSum_ += st.fractionNearRest;
        ++nearRestCount_;
    }
}

void MetricsCollector::sample(const Simulation& sim) {
    if (sim.time() < nextSampleTime_) return;
    nextSampleTime_ = sim.time() + std::max(1.0e-6f, scenario_->duration.outputInterval);

    const auto& particles = sim.particles();
    const size_t begin = sim.boundaryCount();
    const float floorY = scenario_->domain.min.y;

    if (!haveInitialVolume_) {
        report_.initialFluidVolume = sim.stats().fluidVolume;
        haveInitialVolume_ = true;
    }

    // --- surge front -------------------------------------------------
    if (scenario_->metrics.trackSurgeFront) {
        const int axis = std::clamp(scenario_->metrics.surgeAxis, 0, 2);
        // The FRONT is the toe of the surge: the furthest-travelled
        // particle that is still in contact with the floor. Taking the
        // furthest particle overall would track spray instead, which is
        // both noisier and not what Martin & Moyce measured.
        const float contactBand = floorY + 3.0f * sim.spacing();
        float front = -1.0e30f;
        for (size_t i = begin; i < particles.size(); ++i) {
            const glm::vec3& x = particles[i].position;
            if (!std::isfinite(x.x) || !std::isfinite(x.y)) continue;
            if (x.y > contactBand) continue;
            front = std::max(front, x[axis]);
        }
        if (front > -1.0e29f) {
            const float a = scenario_->metrics.surgeColumnWidth;
            SurgeSample s;
            s.time = sim.time();
            s.front = front;
            s.z = a > 0.0f ? (front - scenario_->metrics.surgeOrigin) / a : 0.0f;
            s.tStar = a > 0.0f
                           ? sim.time() * std::sqrt(2.0f * std::abs(scenario_->gravity.y) / a)
                           : 0.0f;
            report_.surge.push_back(s);
        }
    }

    // --- probes -------------------------------------------------------
    for (size_t pi = 0; pi < report_.probes.size(); ++pi) {
        const WaveProbe& spec = scenario_->metrics.probes[pi];
        ProbeSeries& series = report_.probes[pi];
        const float r2 = spec.radius * spec.radius;
        float top = -1.0e30f;
        for (size_t i = begin; i < particles.size(); ++i) {
            const glm::vec3& x = particles[i].position;
            if (!std::isfinite(x.x) || !std::isfinite(x.y) || !std::isfinite(x.z)) continue;
            const float dx = x.x - spec.position.x;
            const float dz = x.z - spec.position.z;
            if (dx * dx + dz * dz > r2) continue;
            top = std::max(top, x.y);
        }
        series.time.push_back(sim.time());
        // An empty column reads as "dry", not as a gap, so the record
        // stays a uniformly sampled time series that can be differenced
        // and zero-crossed without special cases.
        const float elevation = (top > -1.0e29f) ? top : floorY;
        series.elevation.push_back(elevation);
        series.depth.push_back(elevation - floorY);
    }

    // --- inundation ---------------------------------------------------
    if (scenario_->metrics.trackInundation) {
        report_.inundationTracked = true;
        // Floor coverage on a fixed grid: a cell counts as inundated when
        // the tallest fluid particle above it clears the threshold depth.
        // Grid resolution is tied to the particle spacing, so the number
        // is comparable across quality presets only to within one cell --
        // stated in docs/gallery.md rather than presented as exact.
        const glm::vec3 lo = scenario_->domain.min;
        const glm::vec3 hi = scenario_->domain.max;
        const float cell = std::max(4.0f * sim.spacing(), 1.0e-3f);
        const int nx = std::max(1, static_cast<int>((hi.x - lo.x) / cell));
        const int nz = std::max(1, static_cast<int>((hi.z - lo.z) / cell));
        std::vector<float> topY(static_cast<size_t>(nx) * nz, -1.0e30f);
        for (size_t i = begin; i < particles.size(); ++i) {
            const glm::vec3& x = particles[i].position;
            if (!std::isfinite(x.x) || !std::isfinite(x.z)) continue;
            const int ix = std::clamp(static_cast<int>((x.x - lo.x) / cell), 0, nx - 1);
            const int iz = std::clamp(static_cast<int>((x.z - lo.z) / cell), 0, nz - 1);
            float& t = topY[static_cast<size_t>(iz) * nx + ix];
            t = std::max(t, x.y);
        }
        int wet = 0;
        float maxDepth = 0.0f;
        for (float t : topY) {
            if (t < -1.0e29f) continue;
            const float d = t - floorY;
            if (d >= scenario_->metrics.inundationDepth) ++wet;
            maxDepth = std::max(maxDepth, d);
        }
        report_.inundatedAreaFraction =
            static_cast<float>(wet) / static_cast<float>(topY.size());
        report_.maxDepth = std::max(report_.maxDepth, maxDepth);
    }
}

WaveMeasurement MetricsCollector::measureWaveTrain(const std::vector<float>& time,
                                                    const std::vector<float>& elevation,
                                                    float depth, float gravity) {
    WaveMeasurement m;
    if (time.size() < 8 || time.size() != elevation.size()) return m;

    double mean = 0.0;
    for (float e : elevation) mean += e;
    mean /= static_cast<double>(elevation.size());
    m.meanLevel = static_cast<float>(mean);

    // Zero UP-crossings of the de-meaned record. Up-crossings only, so
    // each period is counted once; the interval between consecutive
    // up-crossings is the wave period by definition, and the crest and
    // trough between them give the height. Crossings are linearly
    // interpolated so the period is not quantised to the sample interval.
    std::vector<float> crossings;
    for (size_t i = 1; i < elevation.size(); ++i) {
        const float a = elevation[i - 1] - m.meanLevel;
        const float b = elevation[i] - m.meanLevel;
        if (a <= 0.0f && b > 0.0f) {
            const float u = (b - a) != 0.0f ? (-a) / (b - a) : 0.0f;
            crossings.push_back(time[i - 1] + u * (time[i] - time[i - 1]));
        }
    }
    if (crossings.size() < 2) return m;

    double periodSum = 0.0;
    for (size_t i = 1; i < crossings.size(); ++i) periodSum += crossings[i] - crossings[i - 1];
    m.period = static_cast<float>(periodSum / static_cast<double>(crossings.size() - 1));
    m.wavesCounted = static_cast<int>(crossings.size()) - 1;

    // Height per wave, averaged. Reported as an amplitude (H/2) so it can
    // be compared directly with the commanded paddle amplitude.
    double heightSum = 0.0;
    int heights = 0;
    for (size_t c = 1; c < crossings.size(); ++c) {
        float hi = -1.0e30f, lo = 1.0e30f;
        for (size_t i = 0; i < time.size(); ++i) {
            if (time[i] < crossings[c - 1] || time[i] > crossings[c]) continue;
            hi = std::max(hi, elevation[i]);
            lo = std::min(lo, elevation[i]);
        }
        if (hi > -1.0e29f && lo < 1.0e29f) { heightSum += (hi - lo); ++heights; }
    }
    if (heights > 0) m.amplitude = static_cast<float>(heightSum / heights) * 0.5f;

    if (depth > 0.0f && m.period > 0.0f) {
        const float omega = 2.0f * constants::kPi / m.period;
        const float k = solveDispersion(omega, depth, gravity);
        if (k > 0.0f) {
            m.wavelength = 2.0f * constants::kPi / k;
            m.celerity = m.wavelength / m.period;
            m.steepness = (2.0f * m.amplitude) / m.wavelength;
        }
    }
    m.valid = true;
    return m;
}

WavePrediction MetricsCollector::predictWave(const WaveGenerator& gen, float gravity) {
    WavePrediction p;
    if (gen.stillWaterDepth <= 0.0f || gen.period <= 0.0f) return p;
    if (gen.mode != WaveGenerator::Mode::Sinusoidal &&
        gen.mode != WaveGenerator::Mode::Damped) {
        // Linear monochromatic theory does not describe a single pulse or
        // a superposition, so no prediction is offered rather than one
        // that would be quietly wrong.
        return p;
    }

    p.depth = gen.stillWaterDepth;
    p.period = gen.period;
    const float omega = 2.0f * constants::kPi / gen.period;
    p.wavenumber = solveDispersion(omega, p.depth, gravity);
    if (p.wavenumber <= 0.0f) return p;
    p.wavelength = 2.0f * constants::kPi / p.wavenumber;
    p.celerity = p.wavelength / p.period;

    // Biesel piston transfer function: H/S = 2(cosh(2kd) - 1)/(sinh(2kd) + 2kd),
    // with stroke S = 2 * amplitude.
    const float kd = p.wavenumber * p.depth;
    const float stroke = 2.0f * gen.amplitude;
    const float ratio = 2.0f * (std::cosh(2.0f * kd) - 1.0f) /
                         (std::sinh(2.0f * kd) + 2.0f * kd);
    p.height = ratio * stroke;
    p.steepness = p.height / p.wavelength;
    // Linear theory is a small-amplitude theory. Past roughly H/L = 1/20
    // the prediction is reported but should not be treated as a target.
    p.linearTheoryValid = p.steepness < 0.05f;
    p.valid = true;
    return p;
}

void MetricsCollector::finish(const Simulation& sim, double wallSeconds, double avgStepMs,
                               const std::string& quality, int threads,
                               const std::string& gitRevision) {
    const StepStats& st = sim.stats();
    report_.quality = quality;
    report_.threads = threads;
    report_.gitRevision = gitRevision;
    report_.fluidParticles = st.fluidCount;
    report_.boundaryParticles = st.boundaryCount;
    report_.hitParticleCeiling = sim.hitParticleCeiling();
    report_.smoothingRadius = sim.smoothingRadius();
    report_.spacing = sim.spacing();
    report_.simulatedTime = sim.time();
    report_.steps = sim.stepCount();
    report_.wallTimeSeconds = wallSeconds;
    report_.avgStepMs = avgStepMs;
    report_.dtMean = dtCount_ > 0 ? static_cast<float>(dtSum_ / dtCount_) : 0.0f;
    report_.densityAvgFinal = st.densityAvg;
    report_.fractionNearRestFinal = st.fractionNearRest;
    report_.fractionNearRestMean =
        nearRestCount_ > 0 ? static_cast<float>(nearRestSum_ / nearRestCount_) : 0.0f;
    report_.containmentEvents = st.containmentEvents;
    report_.finalFluidVolume = st.fluidVolume;
    report_.unstableParticles = sim.unstableCount();
    report_.stable = (report_.unstableParticles == 0);

    const float g = std::abs(scenario_->gravity.y);
    for (size_t i = 0; i < report_.probes.size(); ++i) {
        float depth = 0.0f;
        if (!scenario_->waveGenerators.empty()) {
            depth = scenario_->waveGenerators.front().stillWaterDepth;
        }
        if (depth <= 0.0f && !report_.probes[i].depth.empty()) {
            double s = 0.0;
            for (float d : report_.probes[i].depth) s += d;
            depth = static_cast<float>(s / report_.probes[i].depth.size());
        }
        report_.waveMeasurements.push_back(
            measureWaveTrain(report_.probes[i].time, report_.probes[i].elevation, depth, g));
    }
    for (const WaveGenerator& wg : scenario_->waveGenerators) {
        report_.wavePredictions.push_back(predictWave(wg, g));
    }
}

std::string MetricsReport::toJson() const {
    std::ostringstream o;
    o << "{\n";
    o << "  \"scenario\": \"" << jsonEscape(scenario) << "\",\n";
    o << "  \"tier\": \"" << jsonEscape(tier) << "\",\n";
    o << "  \"tier_caveat\": \"" << jsonEscape(tierCaveat) << "\",\n";
    o << "  \"approximation\": \"" << jsonEscape(approximation) << "\",\n";
    o << "  \"git_revision\": \"" << jsonEscape(gitRevision) << "\",\n";
    o << "  \"quality\": \"" << jsonEscape(quality) << "\",\n";
    o << "  \"threads\": " << threads << ",\n";
    o << "  \"status\": \"" << (stable ? "STABLE" : "UNSTABLE") << "\",\n";

    o << "  \"particles\": {\n";
    o << "    \"fluid_final\": " << fluidParticles << ",\n";
    o << "    \"fluid_peak\": " << peakFluidParticles << ",\n";
    o << "    \"boundary\": " << boundaryParticles << ",\n";
    o << "    \"emitted_total\": " << emittedTotal << ",\n";
    o << "    \"removed_total\": " << removedTotal << ",\n";
    o << "    \"hit_ceiling\": " << (hitParticleCeiling ? "true" : "false") << "\n";
    o << "  },\n";

    o << "  \"resolution\": {\n";
    o << "    \"smoothing_radius\": " << num(smoothingRadius) << ",\n";
    o << "    \"spacing\": " << num(spacing) << "\n";
    o << "  },\n";

    o << "  \"time\": {\n";
    o << "    \"simulated_seconds\": " << num(simulatedTime) << ",\n";
    o << "    \"steps\": " << steps << ",\n";
    o << "    \"wall_seconds\": " << num(wallTimeSeconds) << ",\n";
    o << "    \"avg_step_ms\": " << num(avgStepMs) << ",\n";
    o << "    \"dt_min\": " << num(dtMin) << ",\n";
    o << "    \"dt_mean\": " << num(dtMean) << ",\n";
    o << "    \"dt_max\": " << num(dtMax) << "\n";
    o << "  },\n";

    o << "  \"density\": {\n";
    o << "    \"min\": " << num(densityMin) << ",\n";
    o << "    \"max\": " << num(densityMax) << ",\n";
    o << "    \"avg_final\": " << num(densityAvgFinal) << ",\n";
    o << "    \"fraction_within_1pct_final\": " << num(fractionNearRestFinal) << ",\n";
    o << "    \"fraction_within_1pct_mean\": " << num(fractionNearRestMean) << "\n";
    o << "  },\n";

    o << "  \"dynamics\": {\n";
    o << "    \"max_speed\": " << num(maxSpeed) << ",\n";
    o << "    \"max_acceleration\": " << num(maxAccel) << ",\n";
    o << "    \"containment_events\": " << containmentEvents << ",\n";
    o << "    \"unstable_particles\": " << unstableParticles << "\n";
    o << "  },\n";

    o << "  \"volume\": {\n";
    o << "    \"initial_m3\": " << num(initialFluidVolume) << ",\n";
    o << "    \"final_m3\": " << num(finalFluidVolume) << "\n";
    o << "  }";

    if (!surge.empty()) {
        o << ",\n  \"surge_front\": [\n";
        for (size_t i = 0; i < surge.size(); ++i) {
            o << "    {\"t\": " << num(surge[i].time)
              << ", \"front\": " << num(surge[i].front)
              << ", \"Z\": " << num(surge[i].z)
              << ", \"T\": " << num(surge[i].tStar) << "}";
            o << (i + 1 < surge.size() ? ",\n" : "\n");
        }
        o << "  ]";
    }

    if (!probes.empty()) {
        o << ",\n  \"probes\": [\n";
        for (size_t p = 0; p < probes.size(); ++p) {
            o << "    {\n";
            o << "      \"name\": \"" << jsonEscape(probes[p].name) << "\",\n";
            o << "      \"position\": [" << num(probes[p].position.x) << ", "
              << num(probes[p].position.y) << ", " << num(probes[p].position.z) << "],\n";
            if (p < waveMeasurements.size() && waveMeasurements[p].valid) {
                const WaveMeasurement& w = waveMeasurements[p];
                o << "      \"measured\": {\"mean_level\": " << num(w.meanLevel)
                  << ", \"amplitude\": " << num(w.amplitude)
                  << ", \"period\": " << num(w.period)
                  << ", \"waves_counted\": " << w.wavesCounted
                  << ", \"wavelength\": " << num(w.wavelength)
                  << ", \"celerity\": " << num(w.celerity)
                  << ", \"steepness\": " << num(w.steepness) << "},\n";
            }
            o << "      \"series\": {\"t\": [";
            for (size_t i = 0; i < probes[p].time.size(); ++i) {
                o << num(probes[p].time[i]) << (i + 1 < probes[p].time.size() ? ", " : "");
            }
            o << "], \"elevation\": [";
            for (size_t i = 0; i < probes[p].elevation.size(); ++i) {
                o << num(probes[p].elevation[i])
                  << (i + 1 < probes[p].elevation.size() ? ", " : "");
            }
            o << "]}\n    }";
            o << (p + 1 < probes.size() ? ",\n" : "\n");
        }
        o << "  ]";
    }

    if (!wavePredictions.empty()) {
        o << ",\n  \"wave_predictions\": [\n";
        for (size_t i = 0; i < wavePredictions.size(); ++i) {
            const WavePrediction& w = wavePredictions[i];
            o << "    {\"valid\": " << (w.valid ? "true" : "false")
              << ", \"depth\": " << num(w.depth)
              << ", \"period\": " << num(w.period)
              << ", \"wavelength\": " << num(w.wavelength)
              << ", \"celerity\": " << num(w.celerity)
              << ", \"height\": " << num(w.height)
              << ", \"steepness\": " << num(w.steepness)
              << ", \"linear_theory_applicable\": " << (w.linearTheoryValid ? "true" : "false")
              << "}";
            o << (i + 1 < wavePredictions.size() ? ",\n" : "\n");
        }
        o << "  ]";
    }

    if (inundationTracked) {
        o << ",\n  \"inundation\": {\"area_fraction\": " << num(inundatedAreaFraction)
          << ", \"max_depth\": " << num(maxDepth) << "}";
    }

    o << "\n}\n";
    return o.str();
}

bool MetricsReport::writeJson(const std::string& path) const {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << toJson();
    return f.good();
}

} // namespace aquasph
