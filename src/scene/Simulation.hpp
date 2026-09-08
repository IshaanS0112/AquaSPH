#pragma once
#include <memory>
#include <string>
#include <vector>
#include "Scenario.hpp"
#include "../core/Particle.hpp"
#include "../core/SPHKernel.hpp"
#include "../core/DensityPressure.hpp"
#include "../core/ForceCompute.hpp"
#include "../core/Integrator.hpp"
#include "../core/TimeStep.hpp"
#include "../spatial/LinkedCell.hpp"

namespace aquasph {

// Per-step diagnostics. Everything a scenario reports, and everything CI
// asserts on, is derived from these -- which is what makes the scenarios
// experiments rather than animations.
struct StepStats {
    int step = 0;
    float time = 0.0f;
    float dt = 0.0f;
    float dtCfl = 0.0f;
    float dtForce = 0.0f;
    float dtViscous = 0.0f;
    float maxSpeed = 0.0f;
    float maxAccel = 0.0f;
    float densityMin = 0.0f;
    float densityAvg = 0.0f;
    float densityMax = 0.0f;
    // Fraction of fluid particles within 1% of their material's rest
    // density -- the single most informative number about whether the
    // weakly-compressible assumption is actually holding.
    float fractionNearRest = 0.0f;
    int fluidCount = 0;
    int boundaryCount = 0;
    int emittedThisStep = 0;
    int removedThisStep = 0;
    long long containmentEvents = 0;
    double fluidVolume = 0.0;   // m^3, sum of m_i / rho_i
};

// Accumulated wall time per pipeline stage, in milliseconds, for the whole
// run. Off by default: the timers around each stage are cheap but they do
// force the OpenMP regions apart, and a benchmark should measure the
// solver rather than the instrumentation.
//
// The point of measuring per stage rather than per step is Amdahl's law.
// docs/architecture.md predicted, before any of this was threaded, that
// LinkedCell::build would become a growing share of the total as the
// parallel loops sped up. This is how that prediction gets checked with
// numbers instead of argued about.
struct StageProfile {
    double gridBuild = 0.0;
    double boundaryVolumes = 0.0;
    double density = 0.0;
    double normals = 0.0;
    double forces = 0.0;      // includes the integrator's mid-step re-evaluation
    double timestep = 0.0;
    double integrate = 0.0;
    double emitSink = 0.0;
    double stats = 0.0;
    int steps = 0;

    double total() const {
        return gridBuild + boundaryVolumes + density + normals + forces +
               timestep + integrate + emitSink + stats;
    }
};

// Owns the particle array and every scenario primitive, and runs the step
// pipeline. The solver modules under core/ know nothing about scenarios;
// this class is where a Scenario becomes a running simulation.
//
// PARTICLE ARRAY LAYOUT, and why it matters for determinism:
//
//   [0, boundaryCount)            boundary particles, never removed,
//                                 index-stable for the whole run
//   [boundaryCount, size())       fluid particles, appended by emitters,
//                                 compacted in place by sinks
//
// Boundary particles come first so that obstacle motion can address them
// by a fixed index even as the fluid population changes underneath.
// Emitted particles are appended in (emitter index, lattice site index)
// order and sink removal is a stable compaction, so the array contents
// are a pure function of the scenario and the step count -- never of the
// thread count or of allocator behaviour. See docs/architecture.md,
// "Dynamic particle counts".
class Simulation {
public:
    // `maxParticles` is a hard ceiling; emission stops (with one warning)
    // rather than growing without bound, so a mis-specified emitter cannot
    // exhaust memory on a CI runner.
    explicit Simulation(const Scenario& scenario, size_t maxParticles = 4000000);

    // Advances one adaptive timestep. Returns the stats for that step.
    const StepStats& step();

    bool finished() const;

    const std::vector<Particle>& particles() const { return particles_; }
    const Scenario& scenario() const { return scenario_; }
    const StepStats& stats() const { return stats_; }
    float time() const { return time_; }
    int stepCount() const { return stepCount_; }
    float spacing() const { return spacing_; }
    float boundarySpacing() const { return boundarySpacing_; }
    float smoothingRadius() const { return h_; }
    size_t boundaryCount() const { return boundaryCount_; }

    // Boundary particles are laid out as [0, wallCount) domain walls then
    // [wallCount, boundaryCount) obstacle shells. The renderer draws only
    // the second range: drawing the walls would wrap the scene in an
    // opaque box and hide the fluid entirely.
    size_t wallCount() const { return wallCount_; }
    size_t capacity() const { return maxParticles_; }
    bool hitParticleCeiling() const { return hitCeiling_; }

    // Body acceleration currently applied (gravity + external forces).
    glm::vec3 bodyAcceleration() const { return bodyAcceleration_; }

    void enableProfiling(bool on) { profiling_ = on; }
    const StageProfile& profile() const { return profile_; }

    // Number of particles counted as unstable: non-finite coordinates, or
    // outside the domain by more than a tolerance on a face that is not
    // Open. Open faces legitimately lose particles, so counting them as
    // instability would mark every flood scenario UNSTABLE.
    int unstableCount() const;

private:
    Scenario scenario_;
    size_t maxParticles_;

    float h_ = 0.1f;
    float spacing_ = 0.05f;
    float boundarySpacing_ = 0.05f;
    float time_ = 0.0f;
    int stepCount_ = 0;
    float prevDt_ = 0.0f;
    bool hitCeiling_ = false;
    bool anyMovingObstacle_ = false;
    // Obstacle offsets as of the last boundary-volume rebuild. See the
    // note at the recompute site in step() for why the rebuild is driven
    // by displacement rather than run every step.
    std::vector<glm::vec3> lastRebuildOffset_;
    bool needsCompaction_ = false;
    glm::vec3 bodyAcceleration_{0.0f};

    std::vector<Particle> particles_;
    size_t boundaryCount_ = 0;
    size_t wallCount_ = 0;

    // Rest position and owning obstacle for each boundary particle, index
    // -aligned with particles_[0 .. boundaryCount_). Owner -1 = domain wall.
    std::vector<glm::vec3> boundaryRest_;
    std::vector<int> boundaryOwner_;

    // Lattice sites for each emitter, precomputed once so emission order
    // is fixed for the whole run.
    std::vector<std::vector<glm::vec3>> emitterSites_;

    MaterialTable materials_;
    std::vector<TaitEOS> eosTable_;
    std::vector<float> restDensities_;

    std::unique_ptr<CubicSplineKernel> kernel_;
    std::unique_ptr<LinkedCell> grid_;
    std::unique_ptr<PredictorCorrectorIntegrator> integrator_;
    std::unique_ptr<TimeStepController> timestep_;
    ForceParams forceParams_;
    StepStats stats_;
    StageProfile profile_;
    bool profiling_ = false;

    void buildBoundary();
    void buildFluid();
    void updateObstacles(float t);
    void updateBodyAcceleration(float t);
    int runEmitters(float dt);
    int runSinks();
    void refreshStats(const TimeStepInfo& ts, int emitted, int removed);
    bool insideOpenExit(const glm::vec3& p) const;
};

} // namespace aquasph
