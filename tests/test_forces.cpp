#include <gtest/gtest.h>
#include <cmath>
#include "core/Particle.hpp"
#include "core/SPHKernel.hpp"
#include "core/DensityPressure.hpp"
#include "core/ForceCompute.hpp"
#include "core/Material.hpp"
#include "spatial/LinkedCell.hpp"

using namespace aquasph;

namespace {

constexpr float kH = 0.1f;
constexpr float kRho0 = 1000.0f;

MaterialTable waterOnly(float viscosity = 5.0f, float surfaceTension = 0.0f) {
    Material m;
    m.name = "water";
    m.restDensity = kRho0;
    m.soundSpeed = 40.0f;
    m.gamma = 7.0f;
    m.viscosity = viscosity;
    m.surfaceTension = surfaceTension;
    return MaterialTable{m};
}

} // namespace

// REGRESSION TEST FOR A REAL SHIPPED BUG. The v1 viscous term paired
// (v_j - v_i) with r_ij . gradW_ij, which is negative everywhere inside
// the kernel support, so the resulting "viscosity" accelerated particles
// apart in proportion to their relative velocity. Measured directly at
// the time: two particles in pure shear received +86 N along their own
// velocity. A viscous term must always oppose relative motion.
TEST(ForceCompute, ViscosityOpposesRelativeVelocity) {
    CubicSplineKernel kernel(kH);
    const MaterialTable mats = waterOnly();

    std::vector<Particle> ps(2);
    ps[0].position = glm::vec3(0.0f);
    ps[1].position = glm::vec3(0.04f, 0.0f, 0.0f);
    ps[0].velocity = glm::vec3(1.0f, 0.0f, 0.0f);
    ps[1].velocity = glm::vec3(-1.0f, 0.0f, 0.0f);
    for (auto& p : ps) {
        p.mass = kRho0 * 0.05f * 0.05f * 0.05f;
        p.density = kRho0;   // exactly at rest -> zero pressure, isolating viscosity
        p.pressure = 0.0f;
    }

    LinkedCell grid(glm::vec3(-1.0f), glm::vec3(1.0f), kH);
    grid.build(ps);

    ForceParams fp;
    fp.bodyAcceleration = glm::vec3(0.0f);
    fp.materials = &mats;
    computeForces(ps, grid, kernel, fp);

    // i moves in +x, so the viscous force on it must point in -x.
    EXPECT_LT(ps[0].force.x, 0.0f);
    EXPECT_GT(ps[1].force.x, 0.0f);
    // Newton's third law: the pair exchanges momentum, it does not create it.
    EXPECT_NEAR(ps[0].force.x, -ps[1].force.x, std::abs(ps[0].force.x) * 1e-4f + 1e-9f);
}

// REGRESSION TEST FOR THE SECOND HALF OF THE SAME BUG. v1 seeded the
// accumulator with m_i * gravity (a force) and then added the SPH
// pressure term (an acceleration) to it. The integrator divided the total
// by m_i, so every pressure gradient in the simulation came out scaled by
// 1/m_i -- an eleven-fold overstatement at the default scenario's mass.
// Here: the stored force must equal m_i times the textbook acceleration,
// not the acceleration itself.
TEST(ForceCompute, StoresForceNotAcceleration) {
    CubicSplineKernel kernel(kH);
    const MaterialTable mats = waterOnly(/*viscosity=*/0.0f);
    const TaitEOS eos = mats[0].eos();

    const float mass = kRho0 * 0.05f * 0.05f * 0.05f;
    const float rho = 1100.0f;   // compressed -> positive pressure

    std::vector<Particle> ps(2);
    ps[0].position = glm::vec3(0.0f);
    ps[1].position = glm::vec3(0.04f, 0.0f, 0.0f);
    for (auto& p : ps) {
        p.mass = mass;
        p.density = rho;
        p.pressure = eos.pressure(rho);
    }

    LinkedCell grid(glm::vec3(-1.0f), glm::vec3(1.0f), kH);
    grid.build(ps);

    ForceParams fp;
    fp.bodyAcceleration = glm::vec3(0.0f);
    fp.materials = &mats;
    computeForces(ps, grid, kernel, fp);

    const glm::vec3 gradW = kernel.gradW(ps[0].position - ps[1].position);
    const float pTerm = ps[0].pressure / (rho * rho);
    const glm::vec3 expectedAccel = -mass * (2.0f * pTerm) * gradW;
    const glm::vec3 expectedForce = mass * expectedAccel;

    EXPECT_NEAR(ps[0].force.x, expectedForce.x, std::abs(expectedForce.x) * 1e-4f + 1e-12f);
    // And it is emphatically NOT the acceleration: the two differ by m_i,
    // which is far from 1 for any physically-derived particle mass.
    EXPECT_GT(std::abs(expectedAccel.x - expectedForce.x), 1e-6f);
}

TEST(ForceCompute, GravityAppearsAsBodyAcceleration) {
    CubicSplineKernel kernel(kH);
    const MaterialTable mats = waterOnly();

    std::vector<Particle> ps(1);
    ps[0].position = glm::vec3(0.0f);
    ps[0].mass = 0.125f;
    ps[0].density = kRho0;
    ps[0].pressure = 0.0f;

    LinkedCell grid(glm::vec3(-1.0f), glm::vec3(1.0f), kH);
    grid.build(ps);

    ForceParams fp;
    fp.bodyAcceleration = glm::vec3(0.0f, -9.81f, 0.0f);
    fp.materials = &mats;
    computeForces(ps, grid, kernel, fp);

    EXPECT_NEAR(ps[0].force.y, ps[0].mass * -9.81f, 1e-6f);
}

TEST(ForceCompute, PressureForcesConserveMomentum) {
    CubicSplineKernel kernel(kH);
    const MaterialTable mats = waterOnly(/*viscosity=*/0.0f);

    // Asymmetric triangle: the pairwise-symmetric form must still sum to
    // zero net force across the whole system.
    std::vector<Particle> ps(3);
    ps[0].position = glm::vec3(0.00f, 0.00f, 0.0f);
    ps[1].position = glm::vec3(0.04f, 0.01f, 0.0f);
    ps[2].position = glm::vec3(0.01f, 0.05f, 0.0f);
    const float mass = kRho0 * 0.05f * 0.05f * 0.05f;
    for (auto& p : ps) p.mass = mass;

    LinkedCell grid(glm::vec3(-1.0f), glm::vec3(1.0f), kH);
    grid.build(ps);
    computeDensityPressure(ps, grid, kernel, mats[0].eos());

    ForceParams fp;
    fp.bodyAcceleration = glm::vec3(0.0f);
    fp.materials = &mats;
    computeForces(ps, grid, kernel, fp);

    glm::vec3 net(0.0f);
    for (const auto& p : ps) net += p.force;
    const float scale = glm::length(ps[0].force) + 1e-12f;
    EXPECT_NEAR(glm::length(net) / scale, 0.0f, 1e-4f);
}

TEST(ForceCompute, XsphCorrectionPointsTowardNeighbourhoodMean) {
    CubicSplineKernel kernel(kH);
    const MaterialTable mats = waterOnly(/*viscosity=*/0.0f);

    std::vector<Particle> ps(2);
    ps[0].position = glm::vec3(0.0f);
    ps[1].position = glm::vec3(0.03f, 0.0f, 0.0f);
    ps[0].velocity = glm::vec3(0.0f);
    ps[1].velocity = glm::vec3(2.0f, 0.0f, 0.0f);
    for (auto& p : ps) {
        p.mass = kRho0 * 0.05f * 0.05f * 0.05f;
        p.density = kRho0;
        p.pressure = 0.0f;
    }

    LinkedCell grid(glm::vec3(-1.0f), glm::vec3(1.0f), kH);
    grid.build(ps);

    ForceParams fp;
    fp.bodyAcceleration = glm::vec3(0.0f);
    fp.materials = &mats;
    fp.xsphEpsilon = 0.5f;
    computeForces(ps, grid, kernel, fp);

    // The stationary particle is advected slightly toward its faster
    // neighbour, and the fast one slightly toward the slow one.
    EXPECT_GT(ps[0].xsphDelta.x, 0.0f);
    EXPECT_LT(ps[1].xsphDelta.x, 0.0f);
    // XSPH must NOT enter the force: with zero pressure and zero
    // viscosity there is nothing left for the force to be.
    EXPECT_NEAR(glm::length(ps[0].force), 0.0f, 1e-9f);
}

TEST(ForceCompute, XsphDisabledByDefault) {
    CubicSplineKernel kernel(kH);
    const MaterialTable mats = waterOnly();

    std::vector<Particle> ps(2);
    ps[0].position = glm::vec3(0.0f);
    ps[1].position = glm::vec3(0.03f, 0.0f, 0.0f);
    ps[1].velocity = glm::vec3(2.0f, 0.0f, 0.0f);
    for (auto& p : ps) { p.mass = 0.125f; p.density = kRho0; }

    LinkedCell grid(glm::vec3(-1.0f), glm::vec3(1.0f), kH);
    grid.build(ps);

    ForceParams fp;
    fp.materials = &mats;   // xsphEpsilon left at its 0 default
    computeForces(ps, grid, kernel, fp);
    EXPECT_NEAR(glm::length(ps[0].xsphDelta), 0.0f, 1e-12f);
}

TEST(ForceCompute, SurfaceTensionPullsNeighboursTogether) {
    CubicSplineKernel kernel(kH);
    const MaterialTable mats = waterOnly(/*viscosity=*/0.0f, /*surfaceTension=*/1.0f);

    // A small blob: cohesion should give the outermost particle a net
    // inward force even with pressure switched off.
    std::vector<Particle> ps;
    const float s = 0.03f;
    for (int ix = 0; ix < 3; ++ix)
        for (int iy = 0; iy < 3; ++iy) {
            Particle p;
            p.position = glm::vec3(ix * s, iy * s, 0.0f);
            p.mass = kRho0 * s * s * s;
            p.density = kRho0;
            p.pressure = 0.0f;
            ps.push_back(p);
        }

    LinkedCell grid(glm::vec3(-1.0f), glm::vec3(1.0f), kH);
    grid.build(ps);
    computeSurfaceNormals(ps, grid, kernel, mats);

    ForceParams fp;
    fp.bodyAcceleration = glm::vec3(0.0f);
    fp.materials = &mats;
    computeForces(ps, grid, kernel, fp);

    // Corner particle at (0,0); the blob centre is at (s, s).
    const glm::vec3 toCentre = glm::normalize(glm::vec3(s, s, 0.0f) - ps[0].position);
    EXPECT_GT(glm::dot(ps[0].force, toCentre), 0.0f);
}

TEST(ForceCompute, SurfaceTensionOffProducesNoNormals) {
    CubicSplineKernel kernel(kH);
    const MaterialTable mats = waterOnly(/*viscosity=*/0.0f, /*surfaceTension=*/0.0f);

    std::vector<Particle> ps(2);
    ps[0].position = glm::vec3(0.0f);
    ps[1].position = glm::vec3(0.03f, 0.0f, 0.0f);
    for (auto& p : ps) { p.mass = 0.125f; p.density = kRho0; }

    LinkedCell grid(glm::vec3(-1.0f), glm::vec3(1.0f), kH);
    grid.build(ps);
    computeSurfaceNormals(ps, grid, kernel, mats);

    // The pass short-circuits when no material asks for surface tension,
    // so scenarios that do not need it pay nothing.
    EXPECT_NEAR(glm::length(ps[0].normal), 0.0f, 1e-12f);
}
