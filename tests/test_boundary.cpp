// Boundary particles (Akinci et al. 2012) replaced v1's clamp-and-damp
// wall. The property that matters is not "the code runs" but "fluid does
// not pass through a wall, and the wall's strength does not depend on how
// finely it happens to be sampled".
#include <gtest/gtest.h>
#include <cmath>
#include "core/Particle.hpp"
#include "core/SPHKernel.hpp"
#include "core/BoundaryVolume.hpp"
#include "core/DensityPressure.hpp"
#include "core/ForceCompute.hpp"
#include "core/Integrator.hpp"
#include "core/Material.hpp"
#include "core/TimeStep.hpp"
#include "spatial/LinkedCell.hpp"
#include "scene/Shapes.hpp"
#include "scene/Scenario.hpp"
#include "scene/Simulation.hpp"

using namespace aquasph;

namespace {

MaterialTable water(float viscosity = 2.0f) {
    Material m;
    m.name = "water";
    m.restDensity = 1000.0f;
    m.soundSpeed = 20.0f;
    m.viscosity = viscosity;
    return MaterialTable{m};
}

// Total boundary contribution rho_b = sum_b Psi_b * W(r) seen by a probe
// particle sitting one `probeGap` above an infinite flat wall sampled at
// `spacing` with `layers` layers.
float wallDensityContribution(float h, float spacing, int layers, float probeGap) {
    CubicSplineKernel kernel(h);
    std::vector<Particle> ps;

    const int half = static_cast<int>(std::ceil(2.0f * h / spacing)) + 2;
    for (int layer = 0; layer < layers; ++layer) {
        for (int ix = -half; ix <= half; ++ix)
            for (int iz = -half; iz <= half; ++iz) {
                Particle b;
                b.kind = ParticleKind::Boundary;
                b.position = glm::vec3(ix * spacing,
                                        -(layer + 1) * spacing,
                                        iz * spacing);
                ps.push_back(b);
            }
    }
    const size_t boundaryCount = ps.size();

    Particle probe;
    probe.position = glm::vec3(0.0f, probeGap, 0.0f);
    probe.mass = 1000.0f * spacing * spacing * spacing;
    ps.push_back(probe);

    LinkedCell grid(glm::vec3(-2.0f), glm::vec3(2.0f), h);
    grid.build(ps);
    computeBoundaryVolumes(ps, grid, kernel, static_cast<int>(boundaryCount));

    float sum = 0.0f;
    for (size_t i = 0; i < boundaryCount; ++i) {
        const float r = glm::length(ps.back().position - ps[i].position);
        if (r >= h) continue;
        sum += 1000.0f * ps[i].volume * kernel.W(r);
    }
    return sum;
}

} // namespace

// The continuum value the discrete boundary sum is trying to reproduce:
// rho0 times the fraction of the kernel's mass that lies below a plane a
// distance `gap` beneath the probe. Computed here by direct quadrature so
// the test carries its own reference rather than a magic number.
static float halfSpaceLimit(float h, float gap) {
    CubicSplineKernel kernel(h);
    const int n = 160;
    const float step = 2.0f * h / n;
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            for (int k = 0; k < n; ++k) {
                const float x = -h + (i + 0.5f) * step;
                const float y = -h + (j + 0.5f) * step;
                const float z = -h + (k + 0.5f) * step;
                if (y > -gap) continue;              // above the solid surface
                const float r = std::sqrt(x * x + y * y + z * z);
                if (r >= h) continue;
                sum += kernel.W(r) * step * step * step;
            }
    return 1000.0f * static_cast<float>(sum);
}

// WHAT THE AKINCI VOLUME ACTUALLY BUYS, AND WHAT IT DOES NOT.
//
// V_b = 1 / sum_k W(r_bk) is a volume: it must scale with the sampling
// cell s^3, so that Psi_b = rho0 * V_b behaves like a mass no matter how
// the wall was sampled. That is what this test pins down. A boundary
// particle on the outer face of the wall sees roughly half a
// neighbourhood, so its volume lands near 2 s^3 rather than s^3 -- which
// is the whole point, since it has to stand in for the missing half.
TEST(BoundaryParticles, VolumeScalesWithTheSamplingCell) {
    const float h = 0.1f;
    for (float spacing : {0.05f, 0.025f, 0.0125f}) {
        CubicSplineKernel kernel(h);
        std::vector<Particle> ps;
        const int half = static_cast<int>(std::ceil(2.0f * h / spacing)) + 2;
        const int layers = static_cast<int>(std::ceil(2.0f * h / spacing));
        for (int layer = 0; layer < layers; ++layer)
            for (int ix = -half; ix <= half; ++ix)
                for (int iz = -half; iz <= half; ++iz) {
                    Particle b;
                    b.kind = ParticleKind::Boundary;
                    b.position = glm::vec3(ix * spacing, -(layer + 1) * spacing, iz * spacing);
                    ps.push_back(b);
                }
        LinkedCell grid(glm::vec3(-2.0f), glm::vec3(2.0f), h);
        grid.build(ps);
        computeBoundaryVolumes(ps, grid, kernel, static_cast<int>(ps.size()));

        // Deep inside the wall: a full neighbourhood, so V ~ s^3.
        // On the outer face: half a neighbourhood, so V ~ 2 s^3.
        float deepest = 0.0f, surface = 0.0f;
        for (const Particle& b : ps) {
            if (std::abs(b.position.x) > 1e-6f || std::abs(b.position.z) > 1e-6f) continue;
            if (std::abs(b.position.y + spacing) < 1e-6f) surface = b.volume;
            if (std::abs(b.position.y + layers * spacing * 0.5f) < spacing * 0.51f) deepest = b.volume;
        }
        const float cell = spacing * spacing * spacing;
        EXPECT_GT(surface / cell, 1.0f) << "spacing " << spacing;
        EXPECT_LT(surface / cell, 3.0f) << "spacing " << spacing;
        EXPECT_GT(deepest, 0.0f) << "spacing " << spacing;
        EXPECT_LT(deepest / cell, 2.0f) << "spacing " << spacing;
        EXPECT_LT(deepest, surface) << "spacing " << spacing;
    }
}

// WHAT THE WEIGHTING DOES NOT FIX, MEASURED RATHER THAN ASSUMED.
//
// Sampling independence only arrives once enough boundary layers fall
// inside the kernel support, and that count is about h/spacing - 1. At
// this project's h/spacing = 2 a fluid particle resting on a wall sees
// exactly ONE layer, and the boundary sum recovers well under half of the
// half-space limit. It converges from below as the wall is sampled more
// finely. Both halves of that are asserted here, because the shortfall at
// the operating ratio is a real property of the discretisation that the
// documentation states plainly (docs/architecture.md, "How well the
// boundary is resolved") rather than a bug to be hidden behind a loose
// tolerance.
TEST(BoundaryParticles, ContributionConvergesTowardTheHalfSpaceLimitFromBelow) {
    const float h = 0.1f;
    const float gap = 0.01f;
    const float limit = halfSpaceLimit(h, gap);
    ASSERT_GT(limit, 300.0f);
    ASSERT_LT(limit, 600.0f);

    const float ratio2 = wallDensityContribution(h, h / 2.0f, 3, gap);   // h/spacing = 2
    const float ratio4 = wallDensityContribution(h, h / 4.0f, 5, gap);   // h/spacing = 4
    const float ratio8 = wallDensityContribution(h, h / 8.0f, 9, gap);   // h/spacing = 8

    EXPECT_LT(ratio2, ratio4);
    EXPECT_LT(ratio4, ratio8);
    EXPECT_LT(ratio8, limit * 1.15f);
    EXPECT_GT(ratio8, limit * 0.80f);
    // The operating point is genuinely under-resolved, and this records by
    // how much so a future change cannot quietly make it worse.
    EXPECT_LT(ratio2, limit * 0.5f);
}

TEST(BoundaryParticles, ContributionFallsOffWithDistanceFromTheWall) {
    const float h = 0.1f;
    const float near = wallDensityContribution(h, 0.02f, 5, 0.01f);
    const float far  = wallDensityContribution(h, 0.02f, 5, 0.07f);
    EXPECT_GT(near, far);
}

TEST(BoundaryParticles, RepelFluidThatApproachesThem) {
    const float h = 0.08f;
    const float spacing = 0.04f;
    const MaterialTable mats = water();
    CubicSplineKernel kernel(h);

    std::vector<Particle> ps;
    for (int ix = -4; ix <= 4; ++ix)
        for (int iz = -4; iz <= 4; ++iz)
            for (int layer = 0; layer < 2; ++layer) {
                Particle b;
                b.kind = ParticleKind::Boundary;
                b.position = glm::vec3(ix * spacing, -(layer + 1) * spacing, iz * spacing);
                ps.push_back(b);
            }
    const int boundaryCount = static_cast<int>(ps.size());

    Particle f;
    f.position = glm::vec3(0.0f, 0.005f, 0.0f);   // pressed hard against the wall
    f.mass = 1000.0f * spacing * spacing * spacing;
    // Density and pressure are IMPOSED rather than computed here: a single
    // fluid particle over a wall has no fluid above it, so its computed
    // density is necessarily below rest and its pressure would clamp to
    // zero. What is under test is the direction of the boundary pressure
    // force for a compressed particle, so the compression is prescribed
    // and the rest of the pipeline is bypassed.
    f.density = 1080.0f;
    f.pressure = mats[0].eos().pressure(1080.0f);
    ps.push_back(f);
    ASSERT_GT(f.pressure, 0.0f);

    LinkedCell grid(glm::vec3(-1.0f), glm::vec3(1.0f), h);
    grid.build(ps);
    computeBoundaryVolumes(ps, grid, kernel, boundaryCount);

    ForceParams fp;
    fp.bodyAcceleration = glm::vec3(0.0f);
    fp.materials = &mats;
    fp.firstFluidIndex = boundaryCount;
    computeForces(ps, grid, kernel, fp);

    // The wall must push it away from itself (+y), and only that way:
    // an infinite flat wall exerts no tangential pressure force.
    EXPECT_GT(ps.back().force.y, 0.0f);
    EXPECT_NEAR(ps.back().force.x, 0.0f, std::abs(ps.back().force.y) * 1e-3f);
    EXPECT_NEAR(ps.back().force.z, 0.0f, std::abs(ps.back().force.y) * 1e-3f);
}

// THE LEAKAGE TEST THE BOUNDARY WORK EXISTS TO PASS. A column of water is
// dropped onto a solid floor and run well past impact; no particle may
// end up on the far side of the boundary.
TEST(BoundaryParticles, FluidDoesNotLeakThroughTheFloorUnderImpact) {
    Scenario s;
    s.name = "leak_test";
    s.domain.min = glm::vec3(0.0f);
    s.domain.max = glm::vec3(0.24f, 0.30f, 0.24f);
    s.domain.faces[3] = FaceMode::Open;   // open lid, so nothing bounces off it
    s.domain.boundaryParticles = true;
    s.domain.boundaryLayers = 2;

    Material m;
    m.name = "water";
    m.restDensity = 1000.0f;
    m.soundSpeed = 20.0f;
    m.viscosity = 2.0f;
    s.materials = MaterialTable{m};

    // Released from a height, so it hits the floor hard rather than
    // settling gently -- impact is when clamp-style boundaries fail.
    FluidRegion r;
    r.shape.type = ShapeType::Box;
    r.shape.min = glm::vec3(0.04f, 0.14f, 0.04f);
    r.shape.max = glm::vec3(0.20f, 0.28f, 0.20f);
    r.material = "water";
    s.fluidRegions.push_back(r);

    s.numerics.h = 0.02f;
    s.numerics.spacingRatio = 0.5f;
    s.numerics.xsphEpsilon = 0.5f;
    s.duration.simulatedTime = 0.5f;
    s.duration.outputInterval = 0.05f;

    Simulation sim(s);
    ASSERT_GT(sim.stats().fluidCount, 500);
    ASSERT_GT(sim.boundaryCount(), 500u);

    while (!sim.finished()) sim.step();

    // No particle may be more than a hair past a solid face, and the whole
    // configuration must still be finite.
    int leaked = 0;
    const auto& ps = sim.particles();
    for (size_t i = sim.boundaryCount(); i < ps.size(); ++i) {
        const glm::vec3& x = ps[i].position;
        ASSERT_TRUE(std::isfinite(x.x) && std::isfinite(x.y) && std::isfinite(x.z));
        if (x.y < s.domain.min.y - 0.5f * sim.spacing()) ++leaked;
        if (x.x < s.domain.min.x - 0.5f * sim.spacing()) ++leaked;
        if (x.x > s.domain.max.x + 0.5f * sim.spacing()) ++leaked;
        if (x.z < s.domain.min.z - 0.5f * sim.spacing()) ++leaked;
        if (x.z > s.domain.max.z + 0.5f * sim.spacing()) ++leaked;
    }
    EXPECT_EQ(leaked, 0);
    EXPECT_EQ(sim.unstableCount(), 0);
}

TEST(BoundaryParticles, StillWaterStaysStillAndNearRestDensity) {
    // Hydrostatic equilibrium is the cheapest complete statement that the
    // boundary coupling, the pressure law and the timestep controller all
    // agree: still water must stay still, at rest density, without
    // relying on containment.
    Scenario s;
    s.name = "hydrostatic";
    s.domain.min = glm::vec3(0.0f);
    s.domain.max = glm::vec3(0.2f, 0.2f, 0.2f);
    s.domain.faces[3] = FaceMode::Open;
    s.domain.boundaryLayers = 2;

    Material m;
    m.restDensity = 1000.0f;
    m.soundSpeed = 20.0f;
    m.viscosity = 2.0f;
    s.materials = MaterialTable{m};

    FluidRegion r;
    r.shape.type = ShapeType::Box;
    r.shape.min = glm::vec3(0.0f);
    r.shape.max = glm::vec3(0.2f, 0.12f, 0.2f);
    s.fluidRegions.push_back(r);

    s.numerics.h = 0.025f;
    s.numerics.spacingRatio = 0.5f;
    s.numerics.xsphEpsilon = 0.5f;
    s.duration.simulatedTime = 0.6f;
    s.duration.outputInterval = 0.05f;

    Simulation sim(s);
    while (!sim.finished()) sim.step();

    const StepStats& st = sim.stats();
    EXPECT_EQ(sim.unstableCount(), 0);
    EXPECT_NEAR(st.densityAvg, 1000.0f, 20.0f);
    EXPECT_GT(st.fractionNearRest, 0.85f);
    // Residual motion after settling: a few cm/s of numerical noise is
    // expected; anything approaching free-fall speed is not.
    EXPECT_LT(st.maxSpeed, 0.5f);
}
