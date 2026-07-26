#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#ifdef _OPENMP
#include <omp.h>
#endif

#include "core/Particle.hpp"
#include "core/DamBreakInit.hpp"
#include "core/SPHKernel.hpp"
#include "core/DensityPressure.hpp"
#include "core/ForceCompute.hpp"
#include "core/Integrator.hpp"
#include "spatial/LinkedCell.hpp"
#include "io/ConfigLoader.hpp"
#include "benchmark/PerfTimer.hpp"

using namespace aquasph;

namespace {

// Stability check: counts particles with non-finite coordinates (NaN/Inf,
// the telltale sign of a blown-up simulation) or positions outside the
// domain (boundary handling failed). Used both for periodic reporting and
// the final pass/fail verdict.
int countUnstable(const std::vector<Particle>& particles, const glm::vec3& lo, const glm::vec3& hi) {
    int bad = 0;
    const float tol = 1e-3f;
    for (const auto& p : particles) {
        const glm::vec3& x = p.position;
        if (!std::isfinite(x.x) || !std::isfinite(x.y) || !std::isfinite(x.z)) { ++bad; continue; }
        if (x.x < lo.x - tol || x.x > hi.x + tol ||
            x.y < lo.y - tol || x.y > hi.y + tol ||
            x.z < lo.z - tol || x.z > hi.z + tol) {
            ++bad;
        }
    }
    return bad;
}

struct DensityStats { float minD, maxD, avgD; };
DensityStats densityStats(const std::vector<Particle>& particles) {
    float minD = particles.empty() ? 0.0f : particles[0].density;
    float maxD = minD;
    double sum = 0.0;
    for (const auto& p : particles) {
        minD = std::min(minD, p.density);
        maxD = std::max(maxD, p.density);
        sum += p.density;
    }
    const float avgD = particles.empty() ? 0.0f : static_cast<float>(sum / particles.size());
    return {minD, maxD, avgD};
}

struct CliArgs {
    std::string configPath = "configs/default.json";
    int overrideParticles = -1;
    int overrideSteps = -1;
    int overrideReportInterval = -1;
    int overrideThreads = -1;
    bool quiet = false;
};

CliArgs parseArgs(int argc, char** argv) {
    CliArgs args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--particles" && i + 1 < argc) {
            args.overrideParticles = std::stoi(argv[++i]);
        } else if (arg == "--steps" && i + 1 < argc) {
            args.overrideSteps = std::stoi(argv[++i]);
        } else if (arg == "--report-interval" && i + 1 < argc) {
            args.overrideReportInterval = std::stoi(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            args.overrideThreads = std::stoi(argv[++i]);
        } else if (arg == "--config" && i + 1 < argc) {
            args.configPath = argv[++i];
        } else if (arg == "--quiet") {
            args.quiet = true;
        } else if (!arg.empty() && arg[0] != '-') {
            args.configPath = arg;
        }
    }
    return args;
}

} // namespace

int main(int argc, char** argv) {
    const CliArgs args = parseArgs(argc, argv);

    Config cfg = ConfigLoader::load(args.configPath);
    if (args.overrideSteps > 0) cfg.maxSteps = args.overrideSteps;
    if (args.overrideReportInterval > 0) cfg.reportInterval = args.overrideReportInterval;

    const int targetCount = args.overrideParticles > 0 ? args.overrideParticles : cfg.particleCount;
    std::vector<Particle> particles = initializeDamBreak(cfg, targetCount);

#ifdef _OPENMP
    if (args.overrideThreads > 0) {
        omp_set_num_threads(args.overrideThreads);
    }
    const int activeThreads = omp_get_max_threads();
#else
    const int activeThreads = 1;
#endif

    if (!args.quiet) {
        std::cout << "AquaSPH -- Phase 0 physics + Phase 1 OpenMP parallelism\n";
        std::cout << "Particles: " << particles.size()
                  << " | h=" << cfg.h << " | dt=" << cfg.dt
                  << " | c0=" << cfg.soundSpeed
                  << " | steps=" << cfg.maxSteps
                  << " | threads=" << activeThreads << "\n";
    }

    CubicSplineKernel kernel(cfg.h);
    TaitEOS eos(cfg.restDensity, cfg.soundSpeed, cfg.gamma);
    ForceParams forceParams{cfg.viscosity, cfg.gravity};
    BoundaryBox bounds{cfg.domainMin, cfg.domainMax, cfg.wallDamping};
    PredictorCorrectorIntegrator integrator(bounds, cfg.maxSpeed);
    LinkedCell grid(cfg.domainMin, cfg.domainMax, cfg.h);

    const auto recompute = [&](std::vector<Particle>& p) {
        computeForces(p, grid, kernel, forceParams);
    };

    PerfTimer timer;
    for (int step = 0; step < cfg.maxSteps; ++step) {
        timer.start("step");

        grid.build(particles);
        computeDensityPressure(particles, grid, kernel, eos);
        computeForces(particles, grid, kernel, forceParams);
        integrator.step(particles, cfg.dt, recompute);

        timer.stop("step");

        if (!args.quiet && step % cfg.reportInterval == 0) {
            const double avgMs = timer.avgTimeMs("step");
            const double fps = avgMs > 0.0 ? 1000.0 / avgMs : 0.0;
            const int unstable = countUnstable(particles, cfg.domainMin, cfg.domainMax);
            const DensityStats ds = densityStats(particles);
            float minY = particles.empty() ? 0.0f : particles[0].position.y;
            float maxSpeed = 0.0f;
            for (const auto& p : particles) {
                minY = std::min(minY, p.position.y);
                maxSpeed = std::max(maxSpeed, glm::length(p.velocity));
            }
            std::cout << "Step " << std::setw(5) << step
                      << " | avg step: " << std::fixed << std::setprecision(3) << avgMs << " ms"
                      << " | FPS: " << std::setprecision(1) << fps
                      << " | minY: " << std::setprecision(4) << minY
                      << " | maxSpeed: " << maxSpeed
                      << " | rho[min/avg/max]: " << std::setprecision(0) << ds.minD << "/" << ds.avgD << "/" << ds.maxD
                      << " | out-of-bounds/NaN: " << unstable << "\n" << std::flush;
        }
    }

    const int finalUnstable = countUnstable(particles, cfg.domainMin, cfg.domainMax);
    const double avgMs = timer.avgTimeMs("step");
    const double fps = avgMs > 0.0 ? 1000.0 / avgMs : 0.0;

    std::cout << "\n=== Benchmark summary ===\n";
    std::cout << "Particles:                 " << particles.size() << "\n";
    std::cout << "Threads:                    " << activeThreads << "\n";
    std::cout << "Steps:                      " << cfg.maxSteps << "\n";
    std::cout << "Avg step time:              " << avgMs << " ms\n";
    std::cout << "FPS:                        " << fps << "\n";
    std::cout << "Unstable particles at end:  " << finalUnstable << " / " << particles.size() << "\n";
    std::cout << "STATUS: " << (finalUnstable == 0 ? "STABLE" : "UNSTABLE") << "\n" << std::flush;

    return finalUnstable == 0 ? 0 : 1;
}
