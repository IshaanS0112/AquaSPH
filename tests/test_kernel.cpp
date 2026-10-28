#include <gtest/gtest.h>
#include "core/SPHKernel.hpp"
#include "core/Constants.hpp"

using namespace aquasph;

TEST(SPHKernel, NormalizationIntegratesToOne) {
    // Numerically integrate 4*pi*r^2*W(r) dr over [0, h] with a fine midpoint Riemann sum and
    // check it converges to 1.
    const float h = 0.1f;
    CubicSplineKernel kernel(h);

    const int N = 200000;
    const double dr = static_cast<double>(h) / N;
    double integral = 0.0;
    for (int i = 0; i < N; ++i) {
        const double r = (i + 0.5) * dr;
        integral += 4.0 * constants::kPi * r * r * kernel.W(static_cast<float>(r)) * dr;
    }
    EXPECT_NEAR(integral, 1.0, 1e-3);
}

TEST(SPHKernel, ZeroBeyondSupportRadius) {
    const float h = 0.1f;
    CubicSplineKernel kernel(h);
    EXPECT_FLOAT_EQ(kernel.W(h), 0.0f);
    EXPECT_FLOAT_EQ(kernel.W(h * 1.5f), 0.0f);
    EXPECT_FLOAT_EQ(kernel.W(h * 10.0f), 0.0f);
}

TEST(SPHKernel, PositiveWithinSupport) {
    const float h = 0.1f;
    CubicSplineKernel kernel(h);
    EXPECT_GT(kernel.W(0.0f), 0.0f);
    EXPECT_GT(kernel.W(h * 0.25f), 0.0f);
    EXPECT_GT(kernel.W(h * 0.75f), 0.0f);
}

TEST(SPHKernel, MonotonicallyDecreasing) {
    const float h = 0.1f;
    CubicSplineKernel kernel(h);
    float prev = kernel.W(0.0f);
    for (int i = 1; i <= 20; ++i) {
        const float r = h * (static_cast<float>(i) / 20.0f);
        const float w = kernel.W(r);
        EXPECT_LE(w, prev + 1e-6f);
        prev = w;
    }
}

TEST(SPHKernel, GradientSymmetry) {
    const float h = 0.1f;
    CubicSplineKernel kernel(h);
    const glm::vec3 rij(0.03f, -0.02f, 0.01f);

    const glm::vec3 g1 = kernel.gradW(rij);
    const glm::vec3 g2 = kernel.gradW(-rij);

    EXPECT_NEAR(g1.x, -g2.x, 1e-6f);
    EXPECT_NEAR(g1.y, -g2.y, 1e-6f);
    EXPECT_NEAR(g1.z, -g2.z, 1e-6f);
}

TEST(SPHKernel, GradientZeroAtOrigin) {
    const float h = 0.1f;
    CubicSplineKernel kernel(h);
    const glm::vec3 g = kernel.gradW(glm::vec3(0.0f));
    EXPECT_FLOAT_EQ(g.x, 0.0f);
    EXPECT_FLOAT_EQ(g.y, 0.0f);
    EXPECT_FLOAT_EQ(g.z, 0.0f);
}

TEST(SPHKernel, GradientZeroBeyondSupport) {
    const float h = 0.1f;
    CubicSplineKernel kernel(h);
    const glm::vec3 g = kernel.gradW(glm::vec3(h * 2.0f, 0.0f, 0.0f));
    EXPECT_FLOAT_EQ(g.x, 0.0f);
    EXPECT_FLOAT_EQ(g.y, 0.0f);
    EXPECT_FLOAT_EQ(g.z, 0.0f);
}

TEST(SPHKernel, GradientPointsInwardWithinSupport) {
    // The kernel is monotonically decreasing, so grad(W).rij <= 0 -- the
    // gradient always points back toward the "self" particle.
    const float h = 0.1f;
    CubicSplineKernel kernel(h);
    const glm::vec3 rij(0.04f, 0.0f, 0.0f);
    const glm::vec3 g = kernel.gradW(rij);
    EXPECT_LE(glm::dot(g, rij), 0.0f);
}
