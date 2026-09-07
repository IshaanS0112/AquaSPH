#include <gtest/gtest.h>
#include <cmath>
#include "core/Particle.hpp"
#include "core/TimeStep.hpp"

using namespace aquasph;

namespace {
std::vector<Particle> oneParticle(const glm::vec3& v, const glm::vec3& f, float mass = 1.0f) {
    std::vector<Particle> ps(1);
    ps[0].velocity = v;
    ps[0].force = f;
    ps[0].mass = mass;
    return ps;
}
} // namespace

TEST(TimeStep, CflLimitSelectedForSlowFlowWithNoForces) {
    TimeStepParams p;
    p.dtMax = 1.0f;   // out of the way, so the triple itself decides
    TimeStepController ctrl(p, /*h=*/0.1f, /*c0=*/40.0f, /*nu=*/0.0f);

    const auto ps = oneParticle(glm::vec3(0.0f), glm::vec3(0.0f));
    const TimeStepInfo info = ctrl.compute(ps);

    EXPECT_NEAR(info.dt, 0.25f * 0.1f / 40.0f, 1e-9f);
    EXPECT_NEAR(info.dt, info.dtCfl, 1e-9f);
}

TEST(TimeStep, CflTightensAsFlowSpeedRises) {
    TimeStepParams p;
    p.dtMax = 1.0f;
    TimeStepController ctrl(p, 0.1f, 40.0f, 0.0f);

    const float slow = ctrl.compute(oneParticle(glm::vec3(1.0f, 0, 0), glm::vec3(0.0f))).dt;
    const float fast = ctrl.compute(oneParticle(glm::vec3(60.0f, 0, 0), glm::vec3(0.0f))).dt;
    EXPECT_LT(fast, slow);
}

TEST(TimeStep, ForceLimitSelectedUnderLargeAcceleration) {
    TimeStepParams p;
    p.dtMax = 1.0f;
    TimeStepController ctrl(p, 0.1f, 40.0f, 0.0f);

    // a = 1e6 m/s^2 -> dt_force = 0.25*sqrt(0.1/1e6) = 7.9e-5 s, far below
    // the 6.2e-4 s CFL limit for this c0.
    const auto ps = oneParticle(glm::vec3(0.0f), glm::vec3(0.0f, -1.0e6f, 0.0f));
    const TimeStepInfo info = ctrl.compute(ps);

    EXPECT_NEAR(info.dt, info.dtForce, 1e-12f);
    EXPECT_LT(info.dtForce, info.dtCfl);
    EXPECT_NEAR(info.dtForce, 0.25f * std::sqrt(0.1f / 1.0e6f), 1e-9f);
}

TEST(TimeStep, ViscousLimitSelectedAtHighKinematicViscosity) {
    TimeStepParams p;
    p.dtMax = 1.0f;
    // nu = 10 m^2/s -> dt_visc = 0.125 * 0.01 / 10 = 1.25e-4 s
    TimeStepController ctrl(p, 0.1f, 40.0f, /*nu=*/10.0f);

    const TimeStepInfo info = ctrl.compute(oneParticle(glm::vec3(0.0f), glm::vec3(0.0f)));
    EXPECT_NEAR(info.dt, info.dtViscous, 1e-12f);
    EXPECT_NEAR(info.dtViscous, 0.125f * 0.1f * 0.1f / 10.0f, 1e-9f);
}

TEST(TimeStep, ClampedIntoConfiguredBounds) {
    TimeStepParams p;
    p.dtMin = 1.0e-5f;
    p.dtMax = 1.0e-4f;
    TimeStepController ctrl(p, 0.1f, 40.0f, 0.0f);

    // Unforced: the triple wants 6.25e-4 s, above dtMax.
    const TimeStepInfo hi = ctrl.compute(oneParticle(glm::vec3(0.0f), glm::vec3(0.0f)));
    EXPECT_FLOAT_EQ(hi.dt, p.dtMax);
    EXPECT_TRUE(hi.clampedToMax);

    // Absurd acceleration: the triple wants far less than dtMin.
    const TimeStepInfo lo = ctrl.compute(oneParticle(glm::vec3(0.0f), glm::vec3(0.0f, -1.0e14f, 0.0f)));
    EXPECT_FLOAT_EQ(lo.dt, p.dtMin);
    EXPECT_TRUE(lo.clampedToMin);
}

TEST(TimeStep, NonFiniteStateFallsBackToMinimumRatherThanPropagatingNaN) {
    TimeStepParams p;
    TimeStepController ctrl(p, 0.1f, 40.0f, 0.0f);
    const auto ps = oneParticle(glm::vec3(0.0f), glm::vec3(std::nanf(""), 0.0f, 0.0f));
    const TimeStepInfo info = ctrl.compute(ps);
    EXPECT_TRUE(std::isfinite(info.dt));
    EXPECT_FLOAT_EQ(info.dt, p.dtMin);
}

TEST(TimeStep, BoundaryParticlesDoNotConstrainTheStep) {
    TimeStepParams p;
    p.dtMax = 1.0f;
    TimeStepController ctrl(p, 0.1f, 40.0f, 0.0f);

    // A wave paddle sweeping at 50 m/s is prescribed motion, not a fluid
    // stability constraint; letting it drive the CFL limit would let a
    // fast gate throttle the entire simulation.
    std::vector<Particle> ps(1);
    ps[0].kind = ParticleKind::Boundary;
    ps[0].velocity = glm::vec3(50.0f, 0.0f, 0.0f);
    ps[0].force = glm::vec3(0.0f, -1.0e9f, 0.0f);
    ps[0].mass = 1.0f;

    const TimeStepInfo info = ctrl.compute(ps);
    EXPECT_FLOAT_EQ(info.maxSpeed, 0.0f);
    EXPECT_FLOAT_EQ(info.maxAccel, 0.0f);
    EXPECT_NEAR(info.dt, 0.25f * 0.1f / 40.0f, 1e-9f);
}

TEST(TimeStep, OrderOfMagnitudeChangeDetection) {
    EXPECT_TRUE(TimeStepController::isOrderOfMagnitudeChange(1.0e-3f, 1.0e-4f));
    EXPECT_TRUE(TimeStepController::isOrderOfMagnitudeChange(1.0e-5f, 1.0e-4f));
    EXPECT_FALSE(TimeStepController::isOrderOfMagnitudeChange(1.0e-3f, 5.0e-4f));
    EXPECT_FALSE(TimeStepController::isOrderOfMagnitudeChange(0.0f, 1.0e-4f));
}
