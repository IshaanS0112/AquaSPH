#include <gtest/gtest.h>
#include "core/Particle.hpp"
#include "core/SPHKernel.hpp"
#include "core/DensityPressure.hpp"
#include "spatial/LinkedCell.hpp"

using namespace aquasph;

TEST(DensityPressure, SingleParticleDensityMatchesSelfKernelWeight) {
    // A lone particle has no neighbors, so its density estimate reduces to m_i * W(0, h).
    const float h = 0.1f;
    CubicSplineKernel kernel(h);
    std::vector<Particle> particles(1);
    particles[0].position = glm::vec3(0.0f);
    particles[0].mass = 1.0f;

    LinkedCell grid(glm::vec3(-1.0f), glm::vec3(1.0f), h);
    grid.build(particles);

    TaitEOS eos(1000.0f, 40.0f, 7.0f);
    computeDensityPressure(particles, grid, kernel, eos);

    const float expected = particles[0].mass * kernel.W(0.0f);
    EXPECT_NEAR(particles[0].density, expected, 1e-5f);
    EXPECT_NEAR(particles[0].pressure, eos.pressure(expected), 1e-3f);
}

TEST(DensityPressure, UniformLatticeApproximatesRestDensity) {
    // Pack particles on a lattice with mass chosen so mass-per-volume equals rho0, then check
    // the SPH density estimate for an interior (fully-surrounded) particle lands within a
    // generous tolerance of rho0.
    const float h = 0.1f;
    const float spacing = h * 0.5f;
    const float rho0 = 1000.0f;
    const float mass = rho0 * spacing * spacing * spacing;

    CubicSplineKernel kernel(h);
    std::vector<Particle> particles;
    const int n = 11; // interior particle (center) has a full neighborhood on all sides
    for (int ix = 0; ix < n; ++ix)
        for (int iy = 0; iy < n; ++iy)
            for (int iz = 0; iz < n; ++iz) {
                Particle p;
                p.position = glm::vec3(static_cast<float>(ix), static_cast<float>(iy), static_cast<float>(iz)) * spacing;
                p.mass = mass;
                particles.push_back(p);
            }

    const float extent = static_cast<float>(n) * spacing;
    LinkedCell grid(glm::vec3(-0.1f), glm::vec3(extent + 0.1f), h);
    grid.build(particles);

    TaitEOS eos(rho0, 40.0f, 7.0f);
    computeDensityPressure(particles, grid, kernel, eos);

    const int mid = (n / 2) * n * n + (n / 2) * n + (n / 2);
    EXPECT_NEAR(particles[mid].density, rho0, rho0 * 0.25);
}

TEST(TaitEOSTest, ZeroPressureAtRestDensity) {
    TaitEOS eos(1000.0f, 40.0f, 7.0f);
    EXPECT_NEAR(eos.pressure(1000.0f), 0.0f, 1e-3f);
}

TEST(TaitEOSTest, PositivePressureWhenCompressed) {
    TaitEOS eos(1000.0f, 40.0f, 7.0f);
    EXPECT_GT(eos.pressure(1010.0f), 0.0f);
}

TEST(TaitEOSTest, PressureClampedToZeroWhenExpanded) {
    // Below rest density, the raw Tait formula goes negative ("tensile" pressure).
    TaitEOS eos(1000.0f, 40.0f, 7.0f);
    EXPECT_FLOAT_EQ(eos.pressure(990.0f), 0.0f);
    EXPECT_FLOAT_EQ(eos.pressure(500.0f), 0.0f);
}
