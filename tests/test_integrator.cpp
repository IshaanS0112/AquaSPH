#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include "core/Integrator.hpp"

using namespace aquasph;

TEST(Integrator, SingleParticleFollowsParabolicTrajectoryUnderGravity) {
    // Effectively wall-less domain (bounds far away) so the trajectory is
    // unobstructed free fall.
    const BoundaryBox bounds{glm::vec3(-1000.0f), glm::vec3(1000.0f), 1.0f};
    PredictorCorrectorIntegrator integrator(bounds);

    std::vector<Particle> particles(1);
    particles[0].position = glm::vec3(0.0f);
    particles[0].velocity = glm::vec3(0.0f);
    particles[0].mass = 1.0f;
    const glm::vec3 gravity(0.0f, -9.81f, 0.0f);
    particles[0].force = particles[0].mass * gravity;

    // No neighbors, so "recomputing forces" is just re-applying gravity --
    // isolates the integrator from the SPH force pipeline entirely.
    const auto recompute = [&](std::vector<Particle>& p) {
        for (auto& particle : p) particle.force = particle.mass * gravity;
    };

    const float dt = 0.001f;
    const int steps = 100;
    for (int i = 0; i < steps; ++i) {
        integrator.step(particles, dt, recompute);
    }

    const float t = dt * static_cast<float>(steps);
    const float expectedY = 0.5f * gravity.y * t * t; // y = 1/2 g t^2 from rest
    const float expectedVy = gravity.y * t;

    EXPECT_NEAR(particles[0].position.y, expectedY, std::abs(expectedY) * 0.02f + 1e-4f);
    EXPECT_NEAR(particles[0].velocity.y, expectedVy, std::abs(expectedVy) * 0.02f + 1e-4f);
    EXPECT_NEAR(particles[0].position.x, 0.0f, 1e-6f);
    EXPECT_NEAR(particles[0].position.z, 0.0f, 1e-6f);
}

TEST(Integrator, WallReflectionKeepsParticleInBounds) {
    const BoundaryBox bounds{glm::vec3(-1.0f), glm::vec3(1.0f), 0.5f};
    PredictorCorrectorIntegrator integrator(bounds);

    std::vector<Particle> particles(1);
    particles[0].position = glm::vec3(0.0f, -0.99f, 0.0f);
    particles[0].velocity = glm::vec3(0.0f, -5.0f, 0.0f); // moving fast toward the floor
    particles[0].mass = 1.0f;
    particles[0].force = glm::vec3(0.0f, -9.81f, 0.0f);

    const auto recompute = [&](std::vector<Particle>& p) {
        for (auto& particle : p) particle.force = glm::vec3(0.0f, -9.81f, 0.0f) * particle.mass;
    };

    for (int i = 0; i < 50; ++i) {
        integrator.step(particles, 0.001f, recompute);
        EXPECT_GE(particles[0].position.y, bounds.min.y - 1e-4f);
        EXPECT_LE(particles[0].position.y, bounds.max.y + 1e-4f);
    }
}

TEST(Integrator, EnergyDoesNotBlowUpUnderRepeatedSteps) {
    // Rough sanity check, not precise conservation: a particle bouncing in a damped box
    // (damping < 1) should never gain energy above its starting potential energy, since every
    // wall hit removes energy.
    const BoundaryBox bounds{glm::vec3(-1.0f), glm::vec3(1.0f), 0.5f};
    PredictorCorrectorIntegrator integrator(bounds);

    std::vector<Particle> particles(1);
    particles[0].position = glm::vec3(0.0f, 0.9f, 0.0f);
    particles[0].velocity = glm::vec3(0.0f);
    particles[0].mass = 1.0f;
    particles[0].force = glm::vec3(0.0f, -9.81f, 0.0f);

    const auto recompute = [&](std::vector<Particle>& p) {
        for (auto& particle : p) particle.force = glm::vec3(0.0f, -9.81f, 0.0f) * particle.mass;
    };

    float maxEnergy = 0.0f;
    for (int i = 0; i < 5000; ++i) {
        integrator.step(particles, 0.001f, recompute);
        const float ke = 0.5f * particles[0].mass * glm::dot(particles[0].velocity, particles[0].velocity);
        const float pe = particles[0].mass * 9.81f * (particles[0].position.y - bounds.min.y);
        maxEnergy = std::max(maxEnergy, ke + pe);
    }

    const float initialPE = particles[0].mass * 9.81f * (0.9f - bounds.min.y);
    EXPECT_LT(maxEnergy, initialPE * 1.5f); // generous bound -- just catches blow-up
}
