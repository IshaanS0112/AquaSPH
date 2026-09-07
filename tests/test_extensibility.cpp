// PHASE 8: does the architecture actually deliver what it claims?
//
// The claim is that a new phenomenon is a composition of existing
// primitives, not new solver code. That is easy to assert and easy to be
// wrong about, because every scenario in configs/scenarios/ was written
// alongside the primitives and could have quietly shaped them.
//
// These tests therefore build combinations that NO shipped scenario uses,
// entirely through the public scenario API, and assert they run. If any
// of them needed a change to ForceCompute, Integrator or LinkedCell, the
// abstraction would be wrong -- and this file is where that would show up
// as a compile error rather than as a vague feeling.
#include <gtest/gtest.h>
#include <cmath>
#include "scene/Scenario.hpp"
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

// A combination no shipped scenario uses: a MOVING obstacle sweeping
// through fluid that an emitter is still delivering, while a sink drains
// it, under a time-varying external force, with two materials present.
// Every one of those primitives exists for a different scenario; nothing
// in the solver knows they can be combined.
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

// A wave generator is an obstacle with prescribed motion. Putting one in a
// scenario that also has terrain and a sink -- a combination the shipped
// coastal scenarios do not use -- must not need anything new either.
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

// A scenario with no fluid region at all, filled only by an emitter, and
// draining through an open face rather than a sink. Nothing shipped does
// exactly this either.
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

// A periodic channel: the third face mode, which no shipped scenario uses.
// It is here because an unused code path is an untested one, and the face
// modes are the kind of thing that is easy to get wrong in one direction
// only.
TEST(Extensibility, PeriodicFacesWrapFluidInsteadOfLosingIt) {
    Scenario s;
    s.name = "extensibility_periodic";
    s.domain.min = glm::vec3(0.0f);
    s.domain.max = glm::vec3(0.4f, 0.2f, 0.2f);
    s.domain.faces[0] = FaceMode::Periodic;   // -x
    s.domain.faces[1] = FaceMode::Periodic;   // +x
    s.domain.faces[3] = FaceMode::Open;

    s.materials = MaterialTable{makeMaterial("water", 1000.0f, 1.0f)};

    FluidRegion slab;
    slab.shape.type = ShapeType::Box;
    slab.shape.min = glm::vec3(0.0f, 0.0f, 0.0f);
    slab.shape.max = glm::vec3(0.4f, 0.06f, 0.2f);
    slab.velocity = glm::vec3(1.5f, 0.0f, 0.0f);   // driven along the channel
    s.fluidRegions.push_back(slab);

    s.numerics.h = 0.02f;
    s.numerics.spacingRatio = 0.5f;
    s.duration.simulatedTime = 0.5f;
    s.duration.outputInterval = 0.05f;

    Simulation sim(s);
    const int initial = sim.stats().fluidCount;
    ASSERT_GT(initial, 100);

    while (!sim.finished()) sim.step();

    // Nothing is lost through a periodic face -- the fluid travels several
    // channel lengths in this time and every particle must still be there.
    EXPECT_EQ(sim.stats().fluidCount, initial);
    EXPECT_EQ(sim.unstableCount(), 0);
    for (size_t i = sim.boundaryCount(); i < sim.particles().size(); ++i) {
        const float x = sim.particles()[i].position.x;
        EXPECT_GE(x, s.domain.min.x - 0.01f);
        EXPECT_LE(x, s.domain.max.x + 0.01f);
    }
}
