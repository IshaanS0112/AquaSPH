// Scenario primitives. The architectural claim this project makes is that
// a new phenomenon is new JSON plus existing primitives; these tests pin
// down the primitives that claim rests on.
#include <gtest/gtest.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include "scene/Shapes.hpp"
#include "scene/TimeSeries.hpp"
#include "scene/Scenario.hpp"
#include "scene/ScenarioLoader.hpp"
#include "scene/Simulation.hpp"
#include "metrics/Metrics.hpp"

using namespace aquasph;

// --- Shapes -----------------------------------------------------------

TEST(Shapes, BoxContainsAndErodes) {
    Shape s;
    s.type = ShapeType::Box;
    s.min = glm::vec3(0.0f);
    s.max = glm::vec3(1.0f);

    EXPECT_TRUE(s.contains(glm::vec3(0.5f)));
    EXPECT_FALSE(s.contains(glm::vec3(1.5f)));
    EXPECT_TRUE(s.containsEroded(glm::vec3(0.5f), 0.2f));
    EXPECT_FALSE(s.containsEroded(glm::vec3(0.05f, 0.5f, 0.5f), 0.2f));
}

TEST(Shapes, SphereAndCylinderContainment) {
    Shape sphere;
    sphere.type = ShapeType::Sphere;
    sphere.center = glm::vec3(0.0f);
    sphere.radius = 0.5f;
    sphere.min = glm::vec3(-1.0f);
    sphere.max = glm::vec3(1.0f);
    EXPECT_TRUE(sphere.contains(glm::vec3(0.4f, 0.0f, 0.0f)));
    EXPECT_FALSE(sphere.contains(glm::vec3(0.6f, 0.0f, 0.0f)));
    EXPECT_FALSE(sphere.containsEroded(glm::vec3(0.45f, 0.0f, 0.0f), 0.1f));

    Shape cyl;
    cyl.type = ShapeType::Cylinder;
    cyl.center = glm::vec3(0.0f);
    cyl.radius = 0.3f;
    cyl.halfLength = 0.5f;
    cyl.axis = 1;
    cyl.min = glm::vec3(-1.0f);
    cyl.max = glm::vec3(1.0f);
    EXPECT_TRUE(cyl.contains(glm::vec3(0.2f, 0.4f, 0.0f)));
    EXPECT_FALSE(cyl.contains(glm::vec3(0.2f, 0.6f, 0.0f)));   // beyond the end cap
    EXPECT_FALSE(cyl.contains(glm::vec3(0.4f, 0.0f, 0.0f)));   // beyond the radius
}

TEST(Shapes, HeightfieldIsSolidBelowItsSurface) {
    Shape hf;
    hf.type = ShapeType::Heightfield;
    hf.min = glm::vec3(0.0f, 0.0f, 0.0f);
    hf.max = glm::vec3(2.0f, 1.0f, 1.0f);
    hf.field.base = 0.1f;
    hf.field.slope = glm::vec2(0.25f, 0.0f);

    EXPECT_NEAR(hf.field.heightAt(0.0f, 0.0f), 0.1f, 1e-6f);
    EXPECT_NEAR(hf.field.heightAt(1.0f, 0.0f), 0.35f, 1e-6f);
    EXPECT_TRUE(hf.contains(glm::vec3(1.0f, 0.2f, 0.5f)));
    EXPECT_FALSE(hf.contains(glm::vec3(1.0f, 0.5f, 0.5f)));
}

TEST(Shapes, HeightfieldBumpsAddLocally) {
    HeightfieldSpec f;
    f.base = 0.0f;
    HeightfieldSpec::Bump b;
    b.amplitude = 1.0f;
    b.center = glm::vec2(0.0f);
    b.sigma = 0.5f;
    f.bumps.push_back(b);

    EXPECT_NEAR(f.heightAt(0.0f, 0.0f), 1.0f, 1e-5f);
    EXPECT_LT(f.heightAt(2.0f, 0.0f), 0.001f);   // four sigma away
}

TEST(Shapes, VolumeSamplingFillsAndShellSamplingDoesNot) {
    Shape s;
    s.type = ShapeType::Box;
    s.min = glm::vec3(0.0f);
    s.max = glm::vec3(0.5f);

    std::vector<glm::vec3> full, shell;
    sampleVolume(s, 0.05f, full);
    sampleShell(s, 0.05f, 1, shell);

    EXPECT_EQ(full.size(), 11u * 11u * 11u);
    EXPECT_GT(full.size(), shell.size());
    EXPECT_GT(shell.size(), 0u);

    // Asserted as properties rather than an exact count: erosion compares
    // a lattice coordinate against min + d, and for a lattice whose step
    // divides d exactly those are a float ULP apart, so whether the
    // innermost ring lands in the shell is a rounding tie. The shell being
    // one ring thicker than nominal is harmless -- and is the safe
    // direction, since a shell that is too THIN is how fluid leaks.
    for (const glm::vec3& p : shell) EXPECT_TRUE(s.contains(p));
    // The outer skin is definitely in; the centre is definitely out.
    const auto has = [&](const glm::vec3& q) {
        for (const glm::vec3& p : shell) if (glm::length(p - q) < 1e-5f) return true;
        return false;
    };
    EXPECT_TRUE(has(glm::vec3(0.0f)));
    EXPECT_TRUE(has(glm::vec3(0.5f, 0.25f, 0.25f)));
    EXPECT_FALSE(has(glm::vec3(0.25f)));
}

TEST(Shapes, BoxWallsHonourOpenFaces) {
    const glm::vec3 lo(0.0f), hi(0.4f);
    bool allSolid[6] = {true, true, true, true, true, true};
    bool openTop[6] = {true, true, true, false, true, true};

    std::vector<glm::vec3> closed, open;
    sampleBoxWalls(lo, hi, 0.05f, 2, allSolid, closed);
    sampleBoxWalls(lo, hi, 0.05f, 2, openTop, open);

    EXPECT_GT(closed.size(), open.size());
    for (const glm::vec3& p : open) {
        // Nothing may be placed above the open +y face except where it is
        // also outside a solid side wall (a corner), which is deliberate:
        // omitting corners would punch holes through the solid walls.
        if (p.y > hi.y + 0.025f) {
            const bool outsideASolidSide =
                p.x < lo.x - 0.025f || p.x > hi.x + 0.025f ||
                p.z < lo.z - 0.025f || p.z > hi.z + 0.025f;
            EXPECT_TRUE(outsideASolidSide);
        }
    }
}

// --- TimeSeries -------------------------------------------------------

TEST(TimeSeriesTest, ConstantRampAndPulse) {
    TimeSeries c;
    c.kind = TimeSeries::Kind::Constant;
    c.value = 3.0f;
    EXPECT_FLOAT_EQ(c.at(0.0f), 3.0f);
    EXPECT_FLOAT_EQ(c.at(100.0f), 3.0f);

    TimeSeries r;
    r.kind = TimeSeries::Kind::Ramp;
    r.value = 2.0f;
    r.start = 1.0f;
    r.end = 3.0f;
    EXPECT_FLOAT_EQ(r.at(0.5f), 0.0f);
    EXPECT_FLOAT_EQ(r.at(2.0f), 1.0f);
    EXPECT_FLOAT_EQ(r.at(5.0f), 2.0f);

    TimeSeries p;
    p.kind = TimeSeries::Kind::Pulse;
    p.value = 5.0f;
    p.start = 1.0f;
    p.duration = 0.5f;
    EXPECT_FLOAT_EQ(p.at(0.9f), 0.0f);
    EXPECT_FLOAT_EQ(p.at(1.2f), 5.0f);
    EXPECT_FLOAT_EQ(p.at(1.6f), 0.0f);
}

TEST(TimeSeriesTest, SinusoidalRampsUpSmoothlyFromRest) {
    TimeSeries s;
    s.kind = TimeSeries::Kind::Sinusoidal;
    s.value = 1.0f;
    s.period = 1.0f;
    s.rampTime = 2.0f;

    // An impulsive start radiates a spurious transient down the flume, so
    // the envelope must genuinely begin at zero.
    EXPECT_FLOAT_EQ(s.at(0.0f), 0.0f);
    EXPECT_LT(std::abs(s.at(0.25f)), 0.2f);           // still ramping
    EXPECT_NEAR(std::abs(s.at(4.25f)), 1.0f, 1e-4f);  // full amplitude at a crest
}

TEST(TimeSeriesTest, KeyframesInterpolateLinearlyAndHoldAtTheEnds) {
    TimeSeries k;
    k.kind = TimeSeries::Kind::Keyframes;
    k.keys = {glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 10.0f), glm::vec2(2.0f, 5.0f)};
    EXPECT_FLOAT_EQ(k.at(-1.0f), 0.0f);
    EXPECT_FLOAT_EQ(k.at(0.5f), 5.0f);
    EXPECT_FLOAT_EQ(k.at(1.5f), 7.5f);
    EXPECT_FLOAT_EQ(k.at(9.0f), 5.0f);
}

TEST(TimeSeriesTest, DampedDecaysToTheOffset) {
    TimeSeries d;
    d.kind = TimeSeries::Kind::Damped;
    d.value = 1.0f;
    d.period = 0.5f;
    d.decay = 5.0f;
    EXPECT_LT(std::abs(d.at(4.0f)), 1e-6f);
}

// --- Motion (prescribed moving boundaries) ----------------------------

TEST(MotionTest, StaticMotionReportsItselfAsStatic) {
    Motion m;
    EXPECT_TRUE(m.isStatic());
    EXPECT_NEAR(glm::length(m.translationAt(3.0f)), 0.0f, 1e-9f);
    EXPECT_NEAR(glm::length(m.velocityAt(3.0f)), 0.0f, 1e-6f);
}

TEST(MotionTest, OscillatingPaddleVelocityMatchesTheAnalyticDerivative) {
    Motion m;
    m.axis = glm::vec3(1.0f, 0.0f, 0.0f);
    m.displacement.kind = TimeSeries::Kind::Sinusoidal;
    m.displacement.value = 0.05f;
    m.displacement.period = 2.0f;

    // d/dt [A sin(omega t)] = A omega cos(omega t); at t = 0 that is
    // A*omega = 0.05 * pi.
    // Sampled away from t = start: the series is defined as `offset` for
    // t < start, so a central difference exactly at the start time
    // straddles that switch and reports half the true slope. Everywhere
    // else it is the analytic derivative.
    const float omega = 2.0f * 3.14159265f / 2.0f;
    EXPECT_NEAR(m.velocityAt(0.5f).x, 0.0f, 1e-3f);            // stroke extreme
    EXPECT_NEAR(m.velocityAt(1.0f).x, -0.05f * omega, 1e-3f);  // fastest, returning
    EXPECT_NEAR(m.velocityAt(2.0f).x, 0.05f * omega, 1e-3f);   // one period on
    EXPECT_FALSE(m.isStatic());
}

// --- Scenario loading -------------------------------------------------

TEST(ScenarioLoaderTest, RoundTripsACompositionOfPrimitives) {
    const char* kJson = R"JSON({
      "name": "unit_test_scenario",
      "tier": 2,
      "description": "loader coverage",
      "approximation": "none, it is a test",
      "domain": {
        "min": [0, 0, 0], "max": [2, 1, 1],
        "faces": { "x_max": "open", "z_min": "open" },
        "boundary_layers": 3
      },
      "gravity": [0, -9.0, 0],
      "materials": [
        { "name": "water", "rest_density": 1000, "viscosity": 4.0, "surface_tension": 0.1 },
        { "name": "light", "rest_density": 700, "viscosity": 1.0 }
      ],
      "fluid_regions": [
        { "type": "box", "material": "light", "min": [0,0,0], "max": [0.5,0.5,1],
          "profile": "shear", "shear_axis": "y", "shear_rate": 2.0, "shear_dir": [1,0,0] }
      ],
      "emitters": [
        { "type": "box", "min": [0,0,0], "max": [0.1,0.1,1], "direction": [1,0,0],
          "speed": 3.0, "schedule": { "kind": "ramp", "value": 1.0, "start": 0, "end": 1 } }
      ],
      "sinks": [ { "type": "box", "min": [1.9,0,0], "max": [2,1,1] } ],
      "obstacles": [
        { "name": "gate", "type": "box", "min": [1,0,0], "max": [1.1,0.5,1],
          "motion": { "axis": [0,1,0],
                       "displacement": { "kind": "keyframes", "keys": [[0,0],[1,0.5]] } } }
      ],
      "external_forces": [
        { "name": "shake", "direction": [1,0,0],
          "magnitude": { "kind": "sinusoidal", "value": 1.5, "period": 2.0 } }
      ],
      "wave_generators": [
        { "name": "piston", "axis": "x", "position": 0.05, "amplitude": 0.03,
          "period": 1.2, "still_water_depth": 0.4 }
      ],
      "numerics": { "h": 0.05, "spacing_ratio": 0.4, "xsph_epsilon": 0.3 },
      "duration": { "simulated_time": 5.0, "output_interval": 0.05 },
      "render": { "mode": "points", "reference_speed": 7.0 },
      "metrics": { "track_surge_front": true, "surge_column_width": 0.5,
                    "probes": [ { "name": "p1", "position": [1,0,0.5] } ] }
    })JSON";

    const std::string path = "unit_test_scenario.json";
    { std::ofstream f(path); f << kJson; }

    Scenario s;
    std::string err;
    ASSERT_TRUE(ScenarioLoader::loadFile(path, s, err)) << err;
    std::remove(path.c_str());

    EXPECT_EQ(s.name, "unit_test_scenario");
    EXPECT_EQ(s.tier, Tier::Two);
    EXPECT_EQ(s.domain.faces[1], FaceMode::Open);
    EXPECT_EQ(s.domain.faces[4], FaceMode::Open);
    EXPECT_EQ(s.domain.boundaryLayers, 3);
    EXPECT_FLOAT_EQ(s.gravity.y, -9.0f);

    ASSERT_EQ(s.materials.size(), 2u);
    EXPECT_EQ(s.materialIndex("light"), 1);
    EXPECT_FLOAT_EQ(s.materials[0].surfaceTension, 0.1f);

    ASSERT_EQ(s.fluidRegions.size(), 1u);
    EXPECT_EQ(s.fluidRegions[0].profile, FluidRegion::VelocityProfile::Shear);
    EXPECT_EQ(s.fluidRegions[0].shearAxis, 1);

    ASSERT_EQ(s.emitters.size(), 1u);
    EXPECT_FLOAT_EQ(s.emitters[0].speed, 3.0f);
    EXPECT_EQ(s.emitters[0].schedule.kind, TimeSeries::Kind::Ramp);

    ASSERT_EQ(s.sinks.size(), 1u);
    ASSERT_EQ(s.obstacles.size(), 1u);
    EXPECT_FALSE(s.obstacles[0].motion.isStatic());
    ASSERT_EQ(s.externalForces.size(), 1u);
    ASSERT_EQ(s.waveGenerators.size(), 1u);
    EXPECT_FLOAT_EQ(s.waveGenerators[0].stillWaterDepth, 0.4f);

    EXPECT_FLOAT_EQ(s.numerics.spacingRatio, 0.4f);
    EXPECT_EQ(s.render.mode, RenderSettings::Mode::Points);
    EXPECT_FLOAT_EQ(s.render.referenceSpeed, 7.0f);
    EXPECT_TRUE(s.metrics.trackSurgeFront);
    ASSERT_EQ(s.metrics.probes.size(), 1u);
}

// The loader's documented contract is that it reports and falls back
// rather than throwing. Until this was added it did not honour that:
// nlohmann's accessors throw when a field holds the wrong type, nothing
// caught it, and a scenario file with one mistyped field aborted the
// process with a bare `terminate called after throwing` -- the least
// useful possible response to a typo.
TEST(ScenarioLoaderTest, MistypedFieldIsReportedRatherThanThrown) {
    const char* kBad = R"JSON({
      "name": "bad_scenario",
      "domain": { "min": [0, 0, 0], "max": "not an array" },
      "numerics": { "h": "definitely not a number" }
    })JSON";
    const std::string path = "unit_test_bad_scenario.json";
    { std::ofstream f(path); f << kBad; }

    Scenario s;
    std::string err;
    const bool ok = ScenarioLoader::loadFile(path, s, err);
    std::remove(path.c_str());

    EXPECT_FALSE(ok);
    EXPECT_NE(err.find("bad_scenario"), std::string::npos)
        << "the error should name the file it came from; got: " << err;
}

TEST(ScenarioLoaderTest, MalformedJsonIsReportedRatherThanThrown) {
    const std::string path = "unit_test_broken.json";
    { std::ofstream f(path); f << "{ this is not json at all"; }

    Scenario s;
    std::string err;
    const bool ok = ScenarioLoader::loadFile(path, s, err);
    std::remove(path.c_str());

    EXPECT_FALSE(ok);
    EXPECT_FALSE(err.empty());
}

TEST(ScenarioLoaderTest, MissingFileReportsRatherThanThrows) {
    Scenario s;
    std::string err;
    EXPECT_FALSE(ScenarioLoader::loadFile("definitely_not_here.json", s, err));
    EXPECT_FALSE(err.empty());
}

TEST(ScenarioTest, ResolutionScaleKeepsTheHToSpacingRatioFixed) {
    // The whole point of the quality presets: changing resolution must
    // change how finely the fluid is sampled, never how well-conditioned
    // the kernel sums are.
    Numerics n;
    n.h = 0.04f;
    n.spacingRatio = 0.5f;
    for (float scale : {1.8f, 1.0f, 0.55f}) {
        n.resolutionScale = scale;
        EXPECT_NEAR(n.effectiveH() / n.spacing(), 2.0f, 1e-5f);
    }
}

// --- Wave generation --------------------------------------------------

TEST(WaveGeneratorTest, ProducesAPaddleObstacleWithPrescribedMotion) {
    WaveGenerator wg;
    wg.axis = 0;
    wg.position = 0.1f;
    wg.thickness = 0.05f;
    wg.spanMin = glm::vec3(0.0f);
    wg.spanMax = glm::vec3(2.0f, 0.5f, 0.3f);
    wg.mode = WaveGenerator::Mode::Sinusoidal;
    wg.amplitude = 0.02f;
    wg.period = 1.0f;

    const Obstacle o = wg.toObstacle();
    EXPECT_EQ(o.shape.type, ShapeType::Box);
    EXPECT_NEAR(o.shape.min.x, 0.05f, 1e-6f);
    EXPECT_NEAR(o.shape.max.x, 0.10f, 1e-6f);
    EXPECT_FALSE(o.motion.isStatic());
    EXPECT_NEAR(o.motion.axis.x, 1.0f, 1e-6f);
}

TEST(WaveGeneratorTest, PulseModeParksAfterASingleDisplacement) {
    WaveGenerator wg;
    wg.mode = WaveGenerator::Mode::Pulse;
    wg.amplitude = 0.1f;
    wg.period = 0.4f;
    wg.startTime = 0.2f;

    const Obstacle o = wg.toObstacle();
    EXPECT_NEAR(o.motion.displacement.at(0.0f), 0.0f, 1e-6f);
    EXPECT_NEAR(o.motion.displacement.at(0.6f), 0.1f, 1e-5f);
    EXPECT_NEAR(o.motion.displacement.at(5.0f), 0.1f, 1e-5f);   // parked, not oscillating
}

TEST(WavePredictionTest, LinearTheoryMatchesKnownLimits) {
    WaveGenerator wg;
    wg.mode = WaveGenerator::Mode::Sinusoidal;
    wg.amplitude = 0.01f;
    wg.period = 1.0f;
    wg.stillWaterDepth = 0.2f;

    const WavePrediction p = MetricsCollector::predictWave(wg, 9.81f);
    ASSERT_TRUE(p.valid);
    // The dispersion relation must be satisfied by the solved wavenumber.
    const float omega = 2.0f * 3.14159265f / p.period;
    EXPECT_NEAR(9.81f * p.wavenumber * std::tanh(p.wavenumber * p.depth),
                 omega * omega, omega * omega * 1e-3f);
    EXPECT_NEAR(p.celerity, p.wavelength / p.period, 1e-4f);
    // Intermediate depth: celerity must sit between the shallow-water and
    // deep-water limits.
    EXPECT_LT(p.celerity, 9.81f * p.period / (2.0f * 3.14159265f));
    EXPECT_GT(p.celerity, 0.0f);
    EXPECT_LT(p.celerity, std::sqrt(9.81f * p.depth) * 1.001f);
}

TEST(WavePredictionTest, DeclinesToPredictForNonMonochromaticForcing) {
    WaveGenerator wg;
    wg.mode = WaveGenerator::Mode::Pulse;
    wg.amplitude = 0.1f;
    wg.period = 0.4f;
    wg.stillWaterDepth = 0.2f;
    // Linear monochromatic theory does not describe a single pulse, so no
    // number is better than a wrong one.
    EXPECT_FALSE(MetricsCollector::predictWave(wg, 9.81f).valid);
}

TEST(WaveMeasurementTest, RecoversAmplitudeAndPeriodFromASyntheticRecord) {
    const float period = 0.8f;
    const float amplitude = 0.03f;
    std::vector<float> t, eta;
    for (int i = 0; i < 800; ++i) {
        const float ti = i * 0.005f;
        t.push_back(ti);
        eta.push_back(0.2f + amplitude * std::sin(2.0f * 3.14159265f * ti / period));
    }
    const WaveMeasurement m = MetricsCollector::measureWaveTrain(t, eta, 0.2f, 9.81f);
    ASSERT_TRUE(m.valid);
    EXPECT_NEAR(m.period, period, 0.02f);
    EXPECT_NEAR(m.amplitude, amplitude, 0.002f);
    EXPECT_NEAR(m.meanLevel, 0.2f, 0.002f);
    EXPECT_GE(m.wavesCounted, 4);
}

// REGRESSION TEST FOR A REAL MEASUREMENT BUG. A bare mean-crossing test
// counts every ripple that grazes the mean, so a fundamental carrying any
// higher-mode content is reported at a fraction of its true period.
// Measured on the sloshing tank before the hysteresis band was added, the
// two end-wall probes returned 0.554 s and 0.754 s for the same standing
// wave against an analytical 1.26 s. Two probes disagreeing about one
// standing wave is what gave it away.
TEST(WaveMeasurementTest, RejectsRipplesRidingOnTheFundamental) {
    const float fundamental = 1.25f;
    const float amplitude = 0.02f;
    std::vector<float> t, eta;
    for (int i = 0; i < 2000; ++i) {
        const float ti = i * 0.005f;
        t.push_back(ti);
        // A clean fundamental plus a small third-harmonic ripple, large
        // enough to cross the mean repeatedly within each cycle.
        eta.push_back(0.1f
            + amplitude * std::sin(2.0f * 3.14159265f * ti / fundamental)
            + 0.18f * amplitude * std::sin(2.0f * 3.14159265f * ti / (fundamental / 5.0f)));
    }
    const WaveMeasurement m = MetricsCollector::measureWaveTrain(t, eta, 0.1f, 9.81f);
    ASSERT_TRUE(m.valid);
    EXPECT_NEAR(m.period, fundamental, 0.08f)
        << "the ripple was counted as separate waves";
    EXPECT_GE(m.wavesCounted, 5);
}

TEST(WaveMeasurementTest, RefusesToFitAFlatRecord) {
    std::vector<float> t, eta;
    for (int i = 0; i < 100; ++i) { t.push_back(i * 0.01f); eta.push_back(0.2f); }
    const WaveMeasurement m = MetricsCollector::measureWaveTrain(t, eta, 0.2f, 9.81f);
    EXPECT_EQ(m.wavesCounted, 0);
}
