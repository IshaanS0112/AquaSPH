// Emitters and sinks make the particle array grow and shrink during a run.
#include <gtest/gtest.h>
#include <cstring>
#ifdef _OPENMP
#include <omp.h>
#endif
#include "scene/Scenario.hpp"
#include "scene/Simulation.hpp"

using namespace aquasph;

namespace {

Scenario emitterScenario() {
    Scenario s;
    s.name = "emitter_test";
    s.domain.min = glm::vec3(0.0f);
    s.domain.max = glm::vec3(0.3f, 0.4f, 0.3f);
    s.domain.faces[3] = FaceMode::Open;
    s.domain.boundaryLayers = 2;

    Material m;
    m.restDensity = 1000.0f;
    m.soundSpeed = 20.0f;
    m.viscosity = 2.0f;
    s.materials = MaterialTable{m};

    Emitter e;
    e.shape.type = ShapeType::Box;
    e.shape.min = glm::vec3(0.10f, 0.30f, 0.10f);
    e.shape.max = glm::vec3(0.20f, 0.31f, 0.20f);
    e.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    e.speed = 1.0f;
    s.emitters.push_back(e);

    s.numerics.h = 0.03f;
    s.numerics.spacingRatio = 0.5f;
    s.numerics.xsphEpsilon = 0.5f;
    s.duration.simulatedTime = 0.35f;
    s.duration.outputInterval = 0.05f;
    return s;
}

std::vector<Particle> runToEnd(const Scenario& s, int threads) {
#ifdef _OPENMP
    omp_set_num_threads(threads);
#else
    (void)threads;
#endif
    Simulation sim(s);
    while (!sim.finished()) sim.step();
    return sim.particles();
}

} // namespace

TEST(DynamicParticles, EmitterGrowsTheArray) {
    Scenario s = emitterScenario();
    Simulation sim(s);
    const int initial = sim.stats().fluidCount;
    EXPECT_EQ(initial, 0);   // nothing but the emitter

    while (!sim.finished()) sim.step();
    EXPECT_GT(sim.stats().fluidCount, 200);
    EXPECT_EQ(sim.unstableCount(), 0);
}

TEST(DynamicParticles, EmissionIsBitIdenticalAcrossThreadCounts) {
    const Scenario s = emitterScenario();
    const std::vector<Particle> ref = runToEnd(s, 1);
    for (int t : {2, 4, 8}) {
        const std::vector<Particle> run = runToEnd(s, t);
        ASSERT_EQ(run.size(), ref.size()) << "particle count diverged at " << t << " threads";
        EXPECT_EQ(std::memcmp(run.data(), ref.data(), ref.size() * sizeof(Particle)), 0)
            << "particle state diverged at " << t << " threads";
    }
}

TEST(DynamicParticles, SinkRemovesFluidAndKeepsTheRunDeterministic) {
    Scenario s = emitterScenario();
    // A drain across the bottom of the tank: everything the emitter delivers is eventually
    // removed, so the population rises and then holds rather than growing without bound.
    Sink drain;
    drain.shape.type = ShapeType::Box;
    drain.shape.min = glm::vec3(0.0f, 0.0f, 0.0f);
    drain.shape.max = glm::vec3(0.3f, 0.03f, 0.3f);
    s.sinks.push_back(drain);
    s.duration.simulatedTime = 0.6f;

    Simulation sim(s);
    while (!sim.finished()) sim.step();
    EXPECT_GT(sim.stats().fluidCount, 0);
    EXPECT_EQ(sim.unstableCount(), 0);

    const std::vector<Particle> ref = runToEnd(s, 1);
    for (int t : {2, 4}) {
        const std::vector<Particle> run = runToEnd(s, t);
        ASSERT_EQ(run.size(), ref.size());
        EXPECT_EQ(std::memcmp(run.data(), ref.data(), ref.size() * sizeof(Particle)), 0)
            << "sink compaction diverged at " << t << " threads";
    }
}

TEST(DynamicParticles, OpenFaceRemovesOutflowWithoutCountingItUnstable) {
    Scenario s = emitterScenario();
    s.domain.faces[1] = FaceMode::Open;   // +x is an outlet
    Emitter& e = s.emitters[0];
    e.shape.min = glm::vec3(0.02f, 0.05f, 0.10f);
    e.shape.max = glm::vec3(0.03f, 0.20f, 0.20f);
    e.direction = glm::vec3(1.0f, 0.0f, 0.0f);
    e.speed = 2.5f;
    s.duration.simulatedTime = 0.5f;

    Simulation sim(s);
    int removed = 0;
    while (!sim.finished()) removed += sim.step().removedThisStep;

    EXPECT_GT(removed, 0) << "nothing left through the open face";
    // Departing through an Open face is an exit, not a failure.
    EXPECT_EQ(sim.unstableCount(), 0);
}

TEST(DynamicParticles, ParticleCeilingStopsEmissionInsteadOfGrowingWithoutBound) {
    Scenario s = emitterScenario();
    s.duration.simulatedTime = 1.0f;

    // The ceiling is on TOTAL particles, boundary included, so it has to be set above the
    // tank's own wall sampling to be testing emission rather than construction.
    const size_t boundary = Simulation(s).boundaryCount();
    const size_t ceiling = boundary + 400;

    Simulation sim(s, ceiling);
    while (!sim.finished()) sim.step();

    EXPECT_TRUE(sim.hitParticleCeiling());
    EXPECT_LE(sim.particles().size(), ceiling);
    EXPECT_GT(sim.stats().fluidCount, 0);
}

TEST(DynamicParticles, BoundaryParticlesStayAtTheFrontOfTheArray) {
    // Obstacle motion addresses boundary particles by a fixed index while the fluid population
    // changes underneath, so this layout invariant is load-bearing rather than cosmetic.
    Scenario s = emitterScenario();
    Sink drain;
    drain.shape.type = ShapeType::Box;
    drain.shape.min = glm::vec3(0.0f, 0.0f, 0.0f);
    drain.shape.max = glm::vec3(0.3f, 0.03f, 0.3f);
    s.sinks.push_back(drain);

    Simulation sim(s);
    for (int i = 0; i < 200 && !sim.finished(); ++i) sim.step();

    const auto& ps = sim.particles();
    ASSERT_GT(sim.boundaryCount(), 0u);
    for (size_t i = 0; i < sim.boundaryCount(); ++i) {
        EXPECT_EQ(ps[i].kind, ParticleKind::Boundary) << "at index " << i;
    }
    for (size_t i = sim.boundaryCount(); i < ps.size(); ++i) {
        EXPECT_EQ(ps[i].kind, ParticleKind::Fluid) << "at index " << i;
    }
}
