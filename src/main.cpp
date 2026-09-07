// AquaSPH -- headless scenario runner.
//
// Every scenario, Tier 1 and Tier 2 alike, runs through this one entry
// point: `aquasph --scenario <name>`. It reports STABLE/UNSTABLE with a
// matching exit code and can emit a machine-readable metrics file, which
// is what lets CI assert on physics rather than only on "it did not
// crash".
//
// There is no second code path for the dam break any more. v1 had a
// hardcoded initializer plus a flat Config struct; both are gone, and the
// dam break is now configs/scenarios/dam_break.json like everything else.
// Keeping a bespoke path for one scenario is exactly how a "scenario
// engine" quietly becomes a demo with a config file.
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#ifdef _OPENMP
#include <omp.h>
#endif

#include "scene/Scenario.hpp"
#include "scene/ScenarioLoader.hpp"
#include "scene/Simulation.hpp"
#include "scene/Quality.hpp"
#include "metrics/Metrics.hpp"
#include "benchmark/PerfTimer.hpp"

#ifndef AQUASPH_GIT_REV
#define AQUASPH_GIT_REV "unknown"
#endif

using namespace aquasph;

namespace {

struct CliArgs {
    std::string scenario = "dam_break";
    std::string metricsPath;
    Quality quality = Quality::Medium;
    int threads = -1;
    int maxSteps = -1;
    float simulatedTime = -1.0f;
    size_t maxParticles = 4000000;
    bool listScenarios = false;
    bool profile = false;
    bool quiet = false;
    bool help = false;
};

void printUsage() {
    std::cout <<
        "AquaSPH -- SPH fluid solver and scenario laboratory\n\n"
        "Usage: aquasph [options]\n\n"
        "  --scenario NAME      scenario to run (default: dam_break)\n"
        "  --list-scenarios     print available scenarios with their tiers and exit\n"
        "  --quality Q          low | medium | high (default: medium)\n"
        "  --threads N          OpenMP thread count\n"
        "  --time SECONDS       override the scenario's simulated duration\n"
        "  --steps N            hard cap on the number of timesteps\n"
        "  --max-particles N    ceiling on total particles (default: 4000000)\n"
        "  --metrics FILE       write machine-readable metrics JSON\n"
        "  --profile            report wall time per pipeline stage\n"
        "  --quiet              suppress periodic progress output\n"
        "  --help               this message\n\n"
        "Exit code is 0 for STABLE, 1 for UNSTABLE, 2 for a usage or load error.\n";
}

CliArgs parseArgs(int argc, char** argv) {
    CliArgs a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "[aquasph] " << what << " requires a value.\n";
                std::exit(2);
            }
            return argv[++i];
        };
        // std::stoi/stof throw on anything unparseable, and an uncaught
        // exception out of argument parsing means `terminate called after
        // throwing` in response to a typo. Named lambdas so every numeric
        // flag reports which flag and what it received.
        auto asInt = [&](const char* what) -> int {
            const std::string v = next(what);
            try { return std::stoi(v); }
            catch (const std::exception&) {
                std::cerr << "[aquasph] " << what << " expects an integer, got '" << v << "'.\n";
                std::exit(2);
            }
        };
        auto asFloat = [&](const char* what) -> float {
            const std::string v = next(what);
            try { return std::stof(v); }
            catch (const std::exception&) {
                std::cerr << "[aquasph] " << what << " expects a number, got '" << v << "'.\n";
                std::exit(2);
            }
        };
        auto asSize = [&](const char* what) -> size_t {
            const std::string v = next(what);
            try {
                const long long n = std::stoll(v);
                if (n <= 0) throw std::invalid_argument("non-positive");
                return static_cast<size_t>(n);
            } catch (const std::exception&) {
                std::cerr << "[aquasph] " << what << " expects a positive integer, got '"
                          << v << "'.\n";
                std::exit(2);
            }
        };
        if (arg == "--scenario")            a.scenario = next("--scenario");
        else if (arg == "--metrics")        a.metricsPath = next("--metrics");
        else if (arg == "--threads")        a.threads = asInt("--threads");
        else if (arg == "--steps")          a.maxSteps = asInt("--steps");
        else if (arg == "--time")           a.simulatedTime = asFloat("--time");
        else if (arg == "--max-particles")  a.maxParticles = asSize("--max-particles");
        else if (arg == "--list-scenarios") a.listScenarios = true;
        else if (arg == "--profile")        a.profile = true;
        else if (arg == "--quiet")          a.quiet = true;
        else if (arg == "--help" || arg == "-h") a.help = true;
        else if (arg == "--quality") {
            const std::string q = next("--quality");
            if (!parseQuality(q, a.quality)) {
                std::cerr << "[aquasph] Unknown quality '" << q << "'. Use low, medium or high.\n";
                std::exit(2);
            }
        } else {
            std::cerr << "[aquasph] Unknown option '" << arg << "'. Try --help.\n";
            std::exit(2);
        }
    }
    return a;
}

int listScenarios() {
    const auto dirs = ScenarioLoader::defaultSearchDirs();
    const auto names = ScenarioLoader::listAvailable(dirs);
    if (names.empty()) {
        std::cerr << "[aquasph] No scenarios found. Searched:\n";
        for (const auto& d : dirs) std::cerr << "    " << d << "\n";
        return 2;
    }
    std::cout << "Available scenarios (" << names.size() << "):\n\n";
    for (const std::string& n : names) {
        Scenario s;
        std::string err;
        const std::string path = ScenarioLoader::resolve(n, dirs);
        if (path.empty() || !ScenarioLoader::loadFile(path, s, err)) {
            std::cout << "  " << std::left << std::setw(22) << n << "  (failed to load: " << err << ")\n";
            continue;
        }
        std::cout << "  " << std::left << std::setw(22) << n
                  << "  Tier " << static_cast<int>(s.tier) << "  " << s.description << "\n";
    }
    std::cout << "\nTier 1 = physically demonstrable. "
                  "Tier 2 = large-scale visual experiment, not predictive.\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const CliArgs args = parseArgs(argc, argv);
    if (args.help) { printUsage(); return 0; }
    if (args.listScenarios) return listScenarios();

#ifdef _OPENMP
    if (args.threads > 0) omp_set_num_threads(args.threads);
    const int activeThreads = omp_get_max_threads();
#else
    const int activeThreads = 1;
#endif

    const auto dirs = ScenarioLoader::defaultSearchDirs();
    const std::string path = ScenarioLoader::resolve(args.scenario, dirs);
    if (path.empty()) {
        std::cerr << "[aquasph] Scenario '" << args.scenario << "' not found. Searched:\n";
        for (const auto& d : dirs) std::cerr << "    " << d << "\n";
        std::cerr << "Run --list-scenarios to see what is available.\n";
        return 2;
    }

    Scenario scenario;
    std::string error;
    if (!ScenarioLoader::loadFile(path, scenario, error)) {
        std::cerr << "[aquasph] " << error << "\n";
        return 2;
    }

    scenario.numerics.resolutionScale = qualityScale(args.quality);
    if (args.simulatedTime > 0.0f) scenario.duration.simulatedTime = args.simulatedTime;
    if (args.maxSteps > 0) scenario.duration.maxSteps = args.maxSteps;

    Simulation sim(scenario, args.maxParticles);
    sim.enableProfiling(args.profile);
    MetricsCollector metrics(sim.scenario());

    if (!args.quiet) {
        std::cout << "AquaSPH -- " << scenario.name << "\n";
        std::cout << scenario.description << "\n";
        std::cout << tierLabel(scenario.tier) << ". " << tierCaveat(scenario.tier) << "\n";
        if (!scenario.approximation.empty()) {
            std::cout << "Approximation: " << scenario.approximation << "\n";
        }
        std::cout << "quality=" << qualityName(args.quality)
                  << " | h=" << sim.smoothingRadius()
                  << " | spacing=" << sim.spacing()
                  << " | fluid=" << sim.stats().fluidCount
                  << " | boundary=" << sim.boundaryCount()
                  << " | threads=" << activeThreads
                  << " | duration=" << scenario.duration.simulatedTime << " s\n\n";
    }

    if (sim.stats().fluidCount == 0 && scenario.emitters.empty()) {
        std::cerr << "[aquasph] Scenario '" << scenario.name
                  << "' produced no fluid particles and has no emitters. "
                     "Check that its fluid_regions lie inside the domain.\n";
        return 2;
    }

    const auto wallStart = std::chrono::high_resolution_clock::now();
    PerfTimer timer;
    float nextReport = 0.0f;
    const float reportInterval = std::max(scenario.duration.simulatedTime / 10.0f, 1.0e-6f);
    float prevDt = 0.0f;

    while (!sim.finished()) {
        timer.start("step");
        const StepStats& st = sim.step();
        timer.stop("step");

        metrics.observeStep(st);
        metrics.sample(sim);

        if (!args.quiet && TimeStepController::isOrderOfMagnitudeChange(prevDt, st.dt)) {
            // An order-of-magnitude drop in dt marks a real physical event
            // -- impact, wave breaking, jet formation -- and is worth
            // seeing rather than silently absorbing.
            std::cout << "  [dt] t=" << std::fixed << std::setprecision(4) << st.time
                      << " s: " << std::scientific << std::setprecision(2) << prevDt
                      << " -> " << st.dt << " s  (|v|max=" << std::fixed << std::setprecision(2)
                      << st.maxSpeed << " m/s, |a|max=" << st.maxAccel << " m/s^2)\n"
                      << std::defaultfloat;
        }
        prevDt = st.dt;

        if (!args.quiet && st.time >= nextReport) {
            nextReport = st.time + reportInterval;
            std::cout << "t=" << std::fixed << std::setprecision(3) << st.time << " s"
                      << " | step " << std::setw(6) << st.step
                      << " | dt=" << std::scientific << std::setprecision(2) << st.dt
                      << std::fixed << std::setprecision(2)
                      << " | fluid=" << st.fluidCount
                      << " | rho[min/avg/max]=" << std::setprecision(0)
                      << st.densityMin << "/" << st.densityAvg << "/" << st.densityMax
                      << " | within 1%: " << std::setprecision(1)
                      << (100.0f * st.fractionNearRest) << "%"
                      << " | |v|max=" << std::setprecision(2) << st.maxSpeed << " m/s\n"
                      << std::defaultfloat << std::flush;
        }
    }

    const auto wallEnd = std::chrono::high_resolution_clock::now();
    const double wallSeconds = std::chrono::duration<double>(wallEnd - wallStart).count();
    const double avgMs = timer.avgTimeMs("step");

    metrics.finish(sim, wallSeconds, avgMs, qualityName(args.quality), activeThreads,
                    AQUASPH_GIT_REV);
    const MetricsReport& rep = metrics.report();

    std::cout << "\n=== " << scenario.name << " ===\n";
    std::cout << "Tier:                       " << rep.tier << "\n";
    std::cout << "Fluid particles (final):    " << rep.fluidParticles
              << "  (peak " << rep.peakFluidParticles << ")\n";
    std::cout << "Boundary particles:         " << rep.boundaryParticles << "\n";
    if (rep.emittedTotal > 0 || rep.removedTotal > 0) {
        std::cout << "Emitted / removed:          " << rep.emittedTotal
                  << " / " << rep.removedTotal << "\n";
    }
    std::cout << "Threads:                    " << rep.threads << "\n";
    std::cout << "Steps:                      " << rep.steps << "\n";
    std::cout << "Simulated time:             " << rep.simulatedTime << " s\n";
    std::cout << "Wall time:                  " << std::fixed << std::setprecision(2)
              << rep.wallTimeSeconds << " s  (" << std::setprecision(3)
              << rep.avgStepMs << " ms/step)\n" << std::defaultfloat;
    std::cout << "dt [min/mean/max]:          " << std::scientific << std::setprecision(3)
              << rep.dtMin << " / " << rep.dtMean << " / " << rep.dtMax << " s\n"
              << std::defaultfloat;
    std::cout << "Density [min/avg/max]:      " << std::fixed << std::setprecision(1)
              << rep.densityMin << " / " << rep.densityAvgFinal << " / "
              << rep.densityMax << " kg/m^3\n";
    std::cout << "Within 1% of rest density:  " << std::setprecision(1)
              << (100.0f * rep.fractionNearRestFinal) << "% final, "
              << (100.0f * rep.fractionNearRestMean) << "% mean\n";
    std::cout << "Max speed / acceleration:   " << std::setprecision(2) << rep.maxSpeed
              << " m/s  /  " << rep.maxAccel << " m/s^2\n";
    std::cout << "Wall containment events:    " << rep.containmentEvents << "\n";
    std::cout << "Fluid volume [start/end]:   " << std::setprecision(4)
              << rep.initialFluidVolume << " / " << rep.finalFluidVolume << " m^3\n"
              << std::defaultfloat;

    if (!rep.surge.empty()) {
        const SurgeSample& last = rep.surge.back();
        std::cout << "Surge front (final):        Z=" << std::fixed << std::setprecision(3)
                  << last.z << " at T=" << last.tStar << "\n" << std::defaultfloat;
    }
    for (size_t i = 0; i < rep.waveMeasurements.size(); ++i) {
        const WaveMeasurement& w = rep.waveMeasurements[i];
        if (!w.valid || w.wavesCounted < 1) continue;
        std::cout << "Probe '" << rep.probes[i].name << "':           "
                  << std::fixed << std::setprecision(4)
                  << "amplitude=" << w.amplitude << " m, period=" << w.period
                  << " s, waves=" << w.wavesCounted << "\n" << std::defaultfloat;
    }
    for (const WavePrediction& w : rep.wavePredictions) {
        if (!w.valid) continue;
        std::cout << "Linear wavemaker theory:    H=" << std::fixed << std::setprecision(4)
                  << w.height << " m, L=" << w.wavelength << " m, c=" << w.celerity
                  << " m/s, H/L=" << w.steepness
                  << (w.linearTheoryValid ? "" : "  (steepness beyond linear theory)")
                  << "\n" << std::defaultfloat;
    }
    if (rep.inundationTracked) {
        std::cout << "Inundated floor area:       " << std::fixed << std::setprecision(1)
                  << (100.0f * rep.inundatedAreaFraction) << "%  (max depth "
                  << std::setprecision(3) << rep.maxDepth << " m)\n" << std::defaultfloat;
    }
    if (rep.hitParticleCeiling) {
        std::cout << "NOTE: the particle ceiling was reached; emission stopped early.\n";
    }

    std::cout << "STATUS: " << (rep.stable ? "STABLE" : "UNSTABLE")
              << "  (" << rep.unstableParticles << " unstable particles)\n" << std::flush;

    if (args.profile) {
        const StageProfile& pr = sim.profile();
        const double total = pr.total();
        const double n = std::max(1, pr.steps);
        std::cout << "\n=== stage profile (" << pr.steps << " steps, "
                  << activeThreads << " threads, "
                  << rep.fluidParticles << " fluid + " << rep.boundaryParticles
                  << " boundary particles) ===\n";
        struct Row { const char* name; double ms; };
        const Row rows[] = {
            {"linked-cell build", pr.gridBuild},
            {"boundary volumes",  pr.boundaryVolumes},
            {"density + pressure", pr.density},
            {"surface normals",   pr.normals},
            {"forces (x2/step)",  pr.forces},
            {"timestep control",  pr.timestep},
            {"integration",       pr.integrate},
            {"emitters + sinks",  pr.emitSink},
            {"statistics",        pr.stats},
        };
        for (const Row& r : rows) {
            std::cout << "  " << std::left << std::setw(20) << r.name
                      << std::right << std::setw(10) << std::fixed << std::setprecision(3)
                      << (r.ms / n) << " ms/step  "
                      << std::setw(6) << std::setprecision(1)
                      << (total > 0.0 ? 100.0 * r.ms / total : 0.0) << "%\n";
        }
        std::cout << "  " << std::left << std::setw(20) << "TOTAL (measured)"
                  << std::right << std::setw(10) << std::setprecision(3) << (total / n)
                  << " ms/step\n" << std::defaultfloat << std::left;
    }

    if (!args.metricsPath.empty()) {
        if (rep.writeJson(args.metricsPath)) {
            std::cout << "Metrics written to " << args.metricsPath << "\n";
        } else {
            std::cerr << "[aquasph] Failed to write metrics to " << args.metricsPath << "\n";
            return 2;
        }
    }

    return rep.stable ? 0 : 1;
}
