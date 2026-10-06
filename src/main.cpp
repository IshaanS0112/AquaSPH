// AquaSPH -- headless scenario runner.
#include <chrono>
#include <cmath>
#include <csignal>
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

constexpr int kExitStable = 0;
constexpr int kExitUnstable = 1;
constexpr int kExitUsage = 2;
constexpr int kExitCancelled = 3;

// Written only by the signal handler, read once per step by the main loop. sig_atomic_t is the
// one type the standard guarantees can be written from a handler.
volatile std::sig_atomic_t g_stopRequested = 0;

extern "C" void onStopSignal(int sig) {
    if (g_stopRequested) {
        std::signal(sig, SIG_DFL);
        std::raise(sig);
        return;
    }
    g_stopRequested = 1;
}

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
    bool progressJson = false;
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
        "  --progress-json      stdout becomes JSON lines only (start/progress/done)\n"
        "  --help               this message\n\n"
        "Exit code is 0 for STABLE, 1 for UNSTABLE, 2 for a usage or load error,\n"
        "3 for CANCELLED (SIGTERM or SIGINT; metrics are still written).\n";
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
        // std::stoi/stof throw on anything unparseable, and an uncaught exception out of
        // argument parsing means `terminate called after throwing` in response to a typo.
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
        else if (arg == "--progress-json")  a.progressJson = true;
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
    if (a.progressJson && a.profile) {
        // The profile table is human-formatted text on stdout, and
        // --progress-json promises stdout is JSON lines and nothing else.
        std::cerr << "[aquasph] --profile cannot be combined with --progress-json.\n";
        std::exit(2);
    }
    if (a.progressJson) a.quiet = true;
    return a;
}

// One JSON object per line, flushed immediately: stdout is a pipe when the worker reads it, and
// a pipe is block-buffered, so without the flush a consumer would see progress in 4 KiB bursts
// or only at exit.
void emitJsonLine(const std::string& body) {
    std::cout << "{" << body << "}\n" << std::flush;
}

int listScenarios() {
    const auto dirs = ScenarioLoader::defaultSearchDirs();
    const auto names = ScenarioLoader::listAvailable(dirs);
    if (names.empty()) {
        std::cerr << "[aquasph] No scenarios found. Searched:\n";
        for (const auto& d : dirs) std::cerr << "    " << d << "\n";
        return kExitUsage;
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
        return kExitUsage;
    }

    Scenario scenario;
    std::string error;
    if (!ScenarioLoader::loadFile(path, scenario, error)) {
        std::cerr << "[aquasph] " << error << "\n";
        return kExitUsage;
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
        return kExitUsage;
    }

    // Installed only now: a signal during argument parsing or scenario loading has nothing
    // partial worth saving, so the default action (terminate) is the right one there.
    std::signal(SIGTERM, onStopSignal);
    std::signal(SIGINT, onStopSignal);

    if (args.progressJson) {
        emitJsonLine("\"event\":\"start\",\"scenario\":\"" + jsonEscape(scenario.name) +
                     "\",\"tier\":" + std::to_string(static_cast<int>(scenario.tier)) +
                     ",\"quality\":\"" + qualityName(args.quality) +
                     "\",\"fluid\":" + std::to_string(sim.stats().fluidCount) +
                     ",\"boundary\":" + std::to_string(sim.boundaryCount()) +
                     ",\"h\":" + jsonNumber(sim.smoothingRadius()) +
                     ",\"spacing\":" + jsonNumber(sim.spacing()) +
                     ",\"threads\":" + std::to_string(activeThreads) +
                     ",\"t_end\":" + jsonNumber(scenario.duration.simulatedTime));
    }

    const auto wallStart = std::chrono::high_resolution_clock::now();
    PerfTimer timer;
    float nextReport = 0.0f;
    const float reportInterval = std::max(scenario.duration.simulatedTime / 10.0f, 1.0e-6f);
    float prevDt = 0.0f;
    // Progress lines are throttled on wall time, not simulated time or steps: a consumer cares
    // how often it hears from the solver, and a stiff scenario can take thousands of steps per
    // simulated millisecond.
    constexpr double kProgressIntervalS = 0.5;
    double nextProgressWall = kProgressIntervalS;

    while (!sim.finished() && !g_stopRequested) {
        timer.start("step");
        const StepStats& st = sim.step();
        timer.stop("step");

        metrics.observeStep(st);
        metrics.sample(sim);

        if (!args.quiet && TimeStepController::isOrderOfMagnitudeChange(prevDt, st.dt)) {
            // An order-of-magnitude drop in dt marks a real physical event.
            std::cout << "  [dt] t=" << std::fixed << std::setprecision(4) << st.time
                      << " s: " << std::scientific << std::setprecision(2) << prevDt
                      << " -> " << st.dt << " s  (|v|max=" << std::fixed << std::setprecision(2)
                      << st.maxSpeed << " m/s, |a|max=" << st.maxAccel << " m/s^2)\n"
                      << std::defaultfloat;
        }
        prevDt = st.dt;

        if (args.progressJson) {
            const double wallNow = std::chrono::duration<double>(
                std::chrono::high_resolution_clock::now() - wallStart).count();
            if (wallNow >= nextProgressWall) {
                nextProgressWall = wallNow + kProgressIntervalS;
                emitJsonLine("\"event\":\"progress\",\"t\":" + jsonNumber(st.time) +
                             ",\"t_end\":" + jsonNumber(scenario.duration.simulatedTime) +
                             ",\"step\":" + std::to_string(st.step) +
                             ",\"dt\":" + jsonNumber(st.dt) +
                             ",\"fluid\":" + std::to_string(st.fluidCount) +
                             ",\"max_speed\":" + jsonNumber(st.maxSpeed) +
                             ",\"density_max\":" + jsonNumber(st.densityMax) +
                             ",\"wall_s\":" + jsonNumber(wallNow));
            }
        }

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
    const bool cancelled = g_stopRequested != 0;
    metrics.markCancelled(cancelled);
    const MetricsReport& rep = metrics.report();
    const int exitCode = cancelled ? kExitCancelled : (rep.stable ? kExitStable : kExitUnstable);

    if (args.progressJson) {
        // No human summary: stdout is a JSON-lines stream.
        if (!args.metricsPath.empty() && !rep.writeJson(args.metricsPath)) {
            std::cerr << "[aquasph] Failed to write metrics to " << args.metricsPath << "\n";
            return kExitUsage;
        }
        emitJsonLine(std::string("\"event\":\"done\",\"status\":\"") + rep.statusName() +
                     "\",\"t\":" + jsonNumber(rep.simulatedTime) +
                     ",\"steps\":" + std::to_string(rep.steps) +
                     ",\"unstable_particles\":" + std::to_string(rep.unstableParticles) +
                     ",\"wall_s\":" + jsonNumber(rep.wallTimeSeconds) +
                     ",\"exit_code\":" + std::to_string(exitCode));
        return exitCode;
    }

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

    std::cout << "STATUS: " << rep.statusName()
              << "  (" << rep.unstableParticles << " unstable particles)\n" << std::flush;
    if (cancelled) {
        std::cout << "Stopped by signal at t=" << rep.simulatedTime
                  << " s; the figures above describe the run up to that point.\n";
    }

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
            return kExitUsage;
        }
    }

    return exitCode;
}
