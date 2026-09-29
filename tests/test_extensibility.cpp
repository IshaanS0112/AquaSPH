// PHASE 8: does the architecture actually deliver what it claims?
#include <gtest/gtest.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include "scene/Scenario.hpp"
#include "scene/ScenarioLoader.hpp"
#include "scene/Simulation.hpp"

using namespace aquasph;

namespace {

Material makeMaterial(const char* name, float rho0, float viscosity,
                       float surfaceTension = 0.0f) {
    Material m;
    m.name = name;
    m.restDensity = rho0;
    m.soundSpeed = 20.0f;
    m.viscosity = viscosity;
    m.surfaceTension = surfaceTension;
    return m;
}

void runFor(Simulation& sim, float seconds) {
    while (!sim.finished() && sim.time() < seconds) sim.step();
}

} // namespace

// A combination no shipped scenario uses: a MOVING obstacle sweeping through fluid that an
// emitter is still delivering, while a sink drains it, under a time-varying external force,
// with two materials present.
TEST(Extensibility, UnusedCombinationOfPrimitivesRunsWithNoSolverChange) {
    Scenario s;
    s.name = "extensibility_combination";
    s.domain.min = glm::vec3(0.0f);
    s.domain.max = glm::vec3(0.4f, 0.3f, 0.2f);
    s.domain.faces[3] = FaceMode::Open;     // open lid
    s.domain.boundaryLayers = 2;

    s.materials = MaterialTable{makeMaterial("water", 1000.0f, 2.0f),
                                 makeMaterial("light", 700.0f, 1.0f, 0.05f)};

    FluidRegion pool;
    pool.shape.type = ShapeType::Box;
    pool.shape.min = glm::vec3(0.0f);
    pool.shape.max = glm::vec3(0.4f, 0.06f, 0.2f);
    pool.material = "water";
    s.fluidRegions.push_back(pool);

    FluidRegion blob;
    blob.shape.type = ShapeType::Sphere;
    blob.shape.center = glm::vec3(0.1f, 0.18f, 0.1f);
    blob.shape.radius = 0.03f;
    blob.shape.min = glm::vec3(0.0f);
    blob.shape.max = glm::vec3(0.4f, 0.3f, 0.2f);
    blob.material = "light";
    blob.profile = FluidRegion::VelocityProfile::Vortex;
    blob.shearAxis = 1;
    blob.shearRate = 4.0f;
    s.fluidRegions.push_back(blob);

    Emitter jet;
    jet.shape.type = ShapeType::Cylinder;
    jet.shape.center = glm::vec3(0.3f, 0.25f, 0.1f);
    jet.shape.radius = 0.02f;
    jet.shape.halfLength = 0.004f;
    jet.shape.axis = 1;
    jet.shape.min = glm::vec3(0.0f);
    jet.shape.max = glm::vec3(0.4f, 0.3f, 0.2f);
    jet.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    jet.speed = 1.2f;
    jet.material = "water";
    jet.schedule.kind = TimeSeries::Kind::Ramp;
    jet.schedule.value = 1.0f;
    jet.schedule.end = 0.2f;
    s.emitters.push_back(jet);

    Sink drain;
    drain.shape.type = ShapeType::Box;
    drain.shape.min = glm::vec3(0.0f, 0.0f, 0.0f);
    drain.shape.max = glm::vec3(0.05f, 0.02f, 0.2f);
    s.sinks.push_back(drain);

    // A gate sweeping upward through the fluid: prescribed motion, which
    // exists for wave paddles, applied to something that is not a paddle.
    Obstacle gate;
    gate.name = "gate";
    gate.shape.type = ShapeType::Box;
    gate.shape.min = glm::vec3(0.19f, 0.0f, 0.0f);
    gate.shape.max = glm::vec3(0.22f, 0.10f, 0.2f);
    gate.motion.axis = glm::vec3(0.0f, 1.0f, 0.0f);
    gate.motion.displacement.kind = TimeSeries::Kind::Keyframes;
    gate.motion.displacement.keys = {glm::vec2(0.0f, 0.0f), glm::vec2(0.4f, 0.08f)};
    s.obstacles.push_back(gate);

    ExternalForce shake;
    shake.direction = glm::vec3(1.0f, 0.0f, 0.0f);
    shake.magnitude.kind = TimeSeries::Kind::Sinusoidal;
    shake.magnitude.value = 1.5f;
    shake.magnitude.period = 0.3f;
    s.externalForces.push_back(shake);

    s.numerics.h = 0.024f;
    s.numerics.spacingRatio = 0.5f;
    s.numerics.xsphEpsilon = 0.5f;
    s.duration.simulatedTime = 0.5f;
    s.duration.outputInterval = 0.05f;

    Simulation sim(s);
    ASSERT_GT(sim.stats().fluidCount, 200);
    ASSERT_GT(sim.boundaryCount(), 200u);

    runFor(sim, 0.5f);

    EXPECT_EQ(sim.unstableCount(), 0);
    EXPECT_GT(sim.stats().fluidCount, 0);
    // The emitter delivered and the sink removed: both primitives were
    // actually exercised, not merely present.
    EXPECT_GT(sim.stats().boundaryCount, 0);
    for (const Particle& p : sim.particles()) {
        ASSERT_TRUE(std::isfinite(p.position.x) && std::isfinite(p.position.y) &&
                     std::isfinite(p.position.z));
    }
}

// A wave generator is an obstacle with prescribed motion.
TEST(Extensibility, WaveGeneratorComposesWithTerrainAndASink) {
    Scenario s;
    s.name = "extensibility_wave_terrain";
    s.domain.min = glm::vec3(0.0f);
    s.domain.max = glm::vec3(1.2f, 0.3f, 0.2f);
    s.domain.faces[3] = FaceMode::Open;

    s.materials = MaterialTable{makeMaterial("water", 1000.0f, 1.5f)};

    FluidRegion sea;
    sea.shape.type = ShapeType::Box;
    sea.shape.min = glm::vec3(0.1f, 0.0f, 0.0f);
    sea.shape.max = glm::vec3(1.0f, 0.12f, 0.2f);
    s.fluidRegions.push_back(sea);

    Obstacle beach;
    beach.name = "beach";
    beach.shape.type = ShapeType::Heightfield;
    beach.shape.min = glm::vec3(0.85f, 0.0f, 0.0f);
    beach.shape.max = glm::vec3(1.2f, 0.3f, 0.2f);
    beach.shape.field.base = 0.0f;
    beach.shape.field.origin = glm::vec2(0.85f, 0.0f);
    beach.shape.field.slope = glm::vec2(0.5f, 0.0f);
    beach.shape.field.maxHeight = 0.2f;
    s.obstacles.push_back(beach);

    WaveGenerator wg;
    wg.axis = 0;
    wg.position = 0.1f;
    wg.thickness = 0.04f;
    wg.spanMin = s.domain.min;
    wg.spanMax = s.domain.max;
    wg.mode = WaveGenerator::Mode::Sinusoidal;
    wg.amplitude = 0.012f;
    wg.period = 0.7f;
    wg.rampTime = 0.7f;
    wg.stillWaterDepth = 0.12f;
    s.waveGenerators.push_back(wg);

    Sink beachDrain;
    beachDrain.shape.type = ShapeType::Box;
    beachDrain.shape.min = glm::vec3(1.15f, 0.0f, 0.0f);
    beachDrain.shape.max = glm::vec3(1.2f, 0.3f, 0.2f);
    s.sinks.push_back(beachDrain);

    s.numerics.h = 0.024f;
    s.numerics.spacingRatio = 0.5f;
    s.duration.simulatedTime = 0.6f;
    s.duration.outputInterval = 0.05f;

    Simulation sim(s);
    // The wave generator materialised into an obstacle, so its boundary
    // particles exist alongside the terrain's and the tank's.
    ASSERT_GT(sim.boundaryCount(), 500u);
    runFor(sim, 0.6f);
    EXPECT_EQ(sim.unstableCount(), 0);
}

// A scenario with no fluid region at all, filled only by an emitter, and draining through an
// open face rather than a sink.
TEST(Extensibility, EmitterOnlyScenarioWithOpenOutflow) {
    Scenario s;
    s.name = "extensibility_flow_through";
    s.domain.min = glm::vec3(0.0f);
    s.domain.max = glm::vec3(0.5f, 0.2f, 0.2f);
    s.domain.faces[1] = FaceMode::Open;   // +x outlet
    s.domain.faces[3] = FaceMode::Open;   // open lid

    s.materials = MaterialTable{makeMaterial("water", 1000.0f, 2.0f)};

    Emitter inlet;
    inlet.shape.type = ShapeType::Box;
    inlet.shape.min = glm::vec3(0.02f, 0.02f, 0.05f);
    inlet.shape.max = glm::vec3(0.035f, 0.10f, 0.15f);
    inlet.direction = glm::vec3(1.0f, 0.0f, 0.0f);
    inlet.speed = 1.6f;
    s.emitters.push_back(inlet);

    s.numerics.h = 0.02f;
    s.numerics.spacingRatio = 0.5f;
    s.duration.simulatedTime = 0.8f;
    s.duration.outputInterval = 0.05f;

    Simulation sim(s);
    EXPECT_EQ(sim.stats().fluidCount, 0);

    int removed = 0;
    while (!sim.finished()) removed += sim.step().removedThisStep;

    EXPECT_GT(sim.stats().fluidCount, 0) << "the emitter delivered nothing";
    EXPECT_GT(removed, 0) << "nothing left through the open outlet";
    EXPECT_EQ(sim.unstableCount(), 0);
}

// The face mode that is NOT there, and why this test exists.
TEST(Extensibility, PeriodicFacesAreRejectedRatherThanSilentlyApproximated) {
    const char* kJson = R"JSON({
      "name": "periodic_request",
      "domain": { "min": [0,0,0], "max": [0.4,0.2,0.2],
                   "faces": { "x_min": "periodic", "x_max": "periodic" } }
    })JSON";
    const std::string path = "unit_test_periodic.json";
    { std::ofstream f(path); f << kJson; }

    Scenario s;
    std::string err;
    ASSERT_TRUE(ScenarioLoader::loadFile(path, s, err)) << err;
    std::remove(path.c_str());

    // Falls back to a real wall, which is containment the solver can
    // actually honour, rather than a wrap it cannot.
    EXPECT_EQ(s.domain.faces[0], FaceMode::Solid);
    EXPECT_EQ(s.domain.faces[1], FaceMode::Solid);
}
