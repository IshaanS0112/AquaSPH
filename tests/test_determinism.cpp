// The project's headline guarantee: output is bit-identical regardless of thread count.
#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#ifdef _OPENMP
#include <omp.h>
#endif

#include "core/Particle.hpp"
#include "core/ParallelReduce.hpp"
#include "core/SPHKernel.hpp"
#include "core/DensityPressure.hpp"
#include "core/ForceCompute.hpp"
#include "core/Integrator.hpp"
#include "core/Material.hpp"
#include "core/TimeStep.hpp"
#include "spatial/LinkedCell.hpp"

using namespace aquasph;

namespace {

// Thread counts to compare. 8 is included deliberately even on a 4-core machine:
// oversubscription changes scheduling, which is exactly the kind of thing a fragile reduction
// would be sensitive to.
const std::vector<int> kThreadCounts = {1, 2, 4, 8};

void setThreads(int t) {
#ifdef _OPENMP
    omp_set_num_threads(t);
#else
    (void)t;
#endif
}

// A small dam-break-like block, sized so the test stays fast but still
// has many particles per cell and a real collapse under gravity.
std::vector<Particle> makeBlock(int n, float spacing, float rho0) {
    std::vector<Particle> ps;
    ps.reserve(static_cast<size_t>(n) * n * n);
    for (int ix = 0; ix < n; ++ix)
        for (int iy = 0; iy < n; ++iy)
            for (int iz = 0; iz < n; ++iz) {
                Particle p;
                p.position = glm::vec3(-0.45f) + glm::vec3(ix, iy, iz) * spacing;
                p.mass = rho0 * spacing * spacing * spacing;
                p.density = rho0;
                ps.push_back(p);
            }
    return ps;
}

// Bitwise, not approximate. "Close" would pass even if the reduction were
// order-dependent, which is the whole thing being tested.
bool bitIdentical(const std::vector<Particle>& a, const std::vector<Particle>& b) {
    if (a.size() != b.size()) return false;
    return std::memcmp(a.data(), b.data(), a.size() * sizeof(Particle)) == 0;
}

struct RunResult {
    std::vector<Particle> particles;
    std::vector<float> dtHistory;
};

RunResult runSteps(int threads, int steps) {
    setThreads(threads);

    const float h = 0.1f;
    const float spacing = h * 0.5f;
    const float rho0 = 1000.0f;

    Material water;
    water.restDensity = rho0;
    water.soundSpeed = 40.0f;
    water.viscosity = 5.0f;
    water.surfaceTension = 0.2f;
    const MaterialTable mats{water};
    const auto eosTable = makeEosTable(mats);
    const std::vector<float> rho0Table{rho0};

    RunResult result;
    result.particles = makeBlock(10, spacing, rho0);

    CubicSplineKernel kernel(h);
    LinkedCell grid(glm::vec3(-1.0f), glm::vec3(1.0f), h);
    BoundaryBox bounds{glm::vec3(-1.0f), glm::vec3(1.0f), 0.05f};
    PredictorCorrectorIntegrator integrator(bounds);

    ForceParams fp;
    fp.bodyAcceleration = glm::vec3(0.0f, -9.81f, 0.0f);
    fp.materials = &mats;
    fp.xsphEpsilon = 0.5f;

    TimeStepParams tsp;
    TimeStepController timestep(tsp, h, water.soundSpeed, water.viscosity / rho0);

    const auto recompute = [&](std::vector<Particle>& p) {
        computeForces(p, grid, kernel, fp);
    };

    for (int s = 0; s < steps; ++s) {
        grid.build(result.particles);
        computeDensityPressure(result.particles, grid, kernel, eosTable, rho0Table);
        computeSurfaceNormals(result.particles, grid, kernel, mats);
        computeForces(result.particles, grid, kernel, fp);
        const TimeStepInfo ts = timestep.compute(result.particles);
        result.dtHistory.push_back(ts.dt);
        integrator.step(result.particles, ts.dt, recompute);
    }
    return result;
}

} // namespace

TEST(Determinism, DeterministicMaxIsThreadCountIndependent) {
    const int n = 100000;
    std::vector<float> data(n);
    for (int i = 0; i < n; ++i) {
        data[i] = std::sin(static_cast<float>(i) * 0.37f) * static_cast<float>(i % 977);
    }

    float reference = 0.0f;
    bool first = true;
    for (int t : kThreadCounts) {
        setThreads(t);
        const float m = reduce::deterministicMax(n, -1.0e30f, [&](int i) { return data[i]; });
        if (first) { reference = m; first = false; }
        EXPECT_FLOAT_EQ(m, reference) << "thread count " << t;
    }
}

TEST(Determinism, DeterministicSumIsBitIdenticalAcrossThreadCounts) {
    // Values chosen so that float addition genuinely reassociates: mixing magnitudes several
    // orders apart is what makes a naive reduction(+:) thread-dependent in the last bits.
    const int n = 200000;
    std::vector<float> data(n);
    for (int i = 0; i < n; ++i) {
        data[i] = (i % 1000 == 0) ? 1.0e6f : 1.0e-4f * static_cast<float>((i % 13) + 1);
    }

    double reference = 0.0;
    bool first = true;
    for (int t : kThreadCounts) {
        setThreads(t);
        const double s = reduce::deterministicSum(n, [&](int i) { return data[i]; });
        if (first) { reference = s; first = false; }
        EXPECT_EQ(std::memcmp(&s, &reference, sizeof(double)), 0)
            << "thread count " << t << " gave " << s << " vs " << reference;
    }
}

TEST(Determinism, DeterministicCountIsThreadCountIndependent) {
    const int n = 65537;   // deliberately not a multiple of the chunk size
    long long reference = 0;
    bool first = true;
    for (int t : kThreadCounts) {
        setThreads(t);
        const long long c = reduce::deterministicCount(n, [&](int i) { return (i % 7) == 3; });
        if (first) { reference = c; first = false; }
        EXPECT_EQ(c, reference) << "thread count " << t;
    }
}

TEST(Determinism, AdaptiveTimestepSequenceIsThreadCountIndependent) {
    const RunResult ref = runSteps(1, 40);
    for (int t : kThreadCounts) {
        const RunResult run = runSteps(t, 40);
        ASSERT_EQ(run.dtHistory.size(), ref.dtHistory.size());
        for (size_t i = 0; i < ref.dtHistory.size(); ++i) {
            EXPECT_FLOAT_EQ(run.dtHistory[i], ref.dtHistory[i])
                << "thread count " << t << ", step " << i;
        }
    }
}

TEST(Determinism, FullPipelineIsBitIdenticalAcrossThreadCounts) {
    const RunResult ref = runSteps(1, 40);
    for (int t : kThreadCounts) {
        const RunResult run = runSteps(t, 40);
        EXPECT_TRUE(bitIdentical(run.particles, ref.particles))
            << "particle state diverged at thread count " << t;
    }
}
