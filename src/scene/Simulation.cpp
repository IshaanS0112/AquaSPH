#include "Simulation.hpp"
#include "../core/BoundaryVolume.hpp"
#include "../core/ParallelReduce.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace aquasph {

namespace {

// Emission is capped per step so that a pathologically large dt cannot
// dump an unbounded number of layers at once. In a healthy run the
// adaptive timestep keeps this at 0 or 1.
constexpr int kMaxLayersPerStep = 4;

bool faceIsSolid(FaceMode m) { return m == FaceMode::Solid; }

} // namespace

Simulation::Simulation(const Scenario& scenario, size_t maxParticles)
    : scenario_(scenario), maxParticles_(maxParticles) {

    h_ = scenario_.numerics.effectiveH();
    spacing_ = scenario_.numerics.spacing();
    boundarySpacing_ = spacing_ * std::clamp(scenario_.domain.boundarySpacingScale, 0.125f, 4.0f);
    materials_ = scenario_.materials;
    if (materials_.empty()) materials_.push_back(Material{});

    eosTable_ = makeEosTable(materials_);
    restDensities_.reserve(materials_.size());
    for (const Material& m : materials_) restDensities_.push_back(m.restDensity);

    kernel_ = std::make_unique<CubicSplineKernel>(h_);

    // Wave generators are obstacles with prescribed motion; they are
    // materialised into the obstacle list here so that nothing downstream
    // needs a second code path for them. A paddle IS an obstacle.
    for (const WaveGenerator& wg : scenario_.waveGenerators) {
        scenario_.obstacles.push_back(wg.toObstacle());
    }
    for (const Obstacle& o : scenario_.obstacles) {
        if (!o.motion.isStatic()) anyMovingObstacle_ = true;
    }

    // The neighbour grid covers the domain PLUS the wall padding, so wall
    // particles land in their own cells instead of all clamping into the
    // outermost interior cell layer (which would make those buckets large
    // and every query along a wall needlessly expensive).
    const float pad = static_cast<float>(std::max(1, scenario_.domain.boundaryLayers) + 1) * boundarySpacing_;
    grid_ = std::make_unique<LinkedCell>(scenario_.domain.min - glm::vec3(pad),
                                          scenario_.domain.max + glm::vec3(pad), h_);

    BoundaryBox bounds;
    bounds.min = scenario_.domain.min;
    bounds.max = scenario_.domain.max;
    bounds.damping = scenario_.domain.containmentDamping;
    bounds.tolerance = 0.25f * spacing_;
    for (int i = 0; i < 6; ++i) bounds.faces[i] = scenario_.domain.faces[i];
    integrator_ = std::make_unique<PredictorCorrectorIntegrator>(bounds);

    float maxC0 = 0.0f;
    float maxNu = 0.0f;
    for (const Material& m : materials_) {
        maxC0 = std::max(maxC0, m.soundSpeed);
        if (m.restDensity > 0.0f) maxNu = std::max(maxNu, m.viscosity / m.restDensity);
    }
    timestep_ = std::make_unique<TimeStepController>(scenario_.numerics.timestep, h_, maxC0, maxNu);

    forceParams_.viscosity = materials_[0].viscosity;
    forceParams_.xsphEpsilon = scenario_.numerics.xsphEpsilon;
    forceParams_.boundaryFriction = scenario_.numerics.boundaryFriction;
    forceParams_.materials = &materials_;

    for (int i = 0; i < 6; ++i) {
        if (scenario_.domain.faces[i] == FaceMode::Open) needsCompaction_ = true;
    }
    if (!scenario_.sinks.empty()) needsCompaction_ = true;

    buildBoundary();
    buildFluid();

    // One grid build + boundary-volume pass before the run starts. Static
    // geometry does not move, so for most scenarios this is the only time
    // the volumes are ever computed.
    grid_->build(particles_);
    computeBoundaryVolumes(particles_, *grid_, *kernel_, static_cast<int>(boundaryCount_));

    forceParams_.firstFluidIndex = static_cast<int>(boundaryCount_);
    integrator_->setFirstFluidIndex(static_cast<int>(boundaryCount_));

    updateBodyAcceleration(0.0f);
    stats_.boundaryCount = static_cast<int>(boundaryCount_);
    stats_.fluidCount = static_cast<int>(particles_.size() - boundaryCount_);
}

void Simulation::buildBoundary() {
    if (!scenario_.domain.boundaryParticles && scenario_.obstacles.empty()) return;

    std::vector<glm::vec3> points;

    if (scenario_.domain.boundaryParticles) {
        bool solid[6];
        for (int i = 0; i < 6; ++i) solid[i] = faceIsSolid(scenario_.domain.faces[i]);
        // Layers are counted in BOUNDARY spacings, so halving the
        // boundary spacing at a fixed layer count halves the wall's
        // physical thickness too. Scale the count so the wall keeps the
        // same thickness in metres, which is what actually determines
        // whether fluid can tunnel through it.
        const int layers = std::max(
            1, static_cast<int>(std::lround(scenario_.domain.boundaryLayers * spacing_ / boundarySpacing_)));
        sampleBoxWalls(scenario_.domain.min, scenario_.domain.max, boundarySpacing_,
                        layers, solid, points);
    }

    for (const glm::vec3& p : points) {
        Particle bp;
        bp.position = p;
        bp.kind = ParticleKind::Boundary;
        particles_.push_back(bp);
        boundaryRest_.push_back(p);
        boundaryOwner_.push_back(-1);
    }

    wallCount_ = particles_.size();

    for (size_t oi = 0; oi < scenario_.obstacles.size(); ++oi) {
        std::vector<glm::vec3> shell;
        const int layers = std::max(
            1, static_cast<int>(std::lround(scenario_.domain.boundaryLayers * spacing_ / boundarySpacing_)));
        sampleShell(scenario_.obstacles[oi].shape, boundarySpacing_, layers, shell);
        for (const glm::vec3& p : shell) {
            Particle bp;
            bp.position = p;
            bp.kind = ParticleKind::Boundary;
            particles_.push_back(bp);
            boundaryRest_.push_back(p);
            boundaryOwner_.push_back(static_cast<int>(oi));
        }
    }

    boundaryCount_ = particles_.size();
}

void Simulation::buildFluid() {
    // Reserve for the initial fluid plus a generous estimate of what the
    // emitters will add, so the array does not reallocate mid-run. This is
    // an optimisation, not a correctness requirement -- reallocation would
    // be safe (nothing holds a Particle* across a step) but it would show
    // up as periodic step-time spikes in the benchmark.
    size_t estimate = particles_.size();
    for (const FluidRegion& r : scenario_.fluidRegions) {
        estimate += static_cast<size_t>(r.shape.approximateVolume() /
                                         (spacing_ * spacing_ * spacing_));
    }
    for (const Emitter& e : scenario_.emitters) {
        const float area = e.shape.approximateVolume() / std::max(spacing_, 1.0e-6f);
        const float sites = area / (spacing_ * spacing_);
        const float layers = e.speed * scenario_.duration.simulatedTime / spacing_;
        estimate += static_cast<size_t>(std::max(0.0f, sites * layers));
    }
    particles_.reserve(std::min(maxParticles_, estimate + 1024));

    for (const FluidRegion& region : scenario_.fluidRegions) {
        std::vector<glm::vec3> sites;
        sampleVolume(region.shape, spacing_, sites);
        if (sites.empty()) continue;

        const std::uint8_t mat = scenario_.materialIndex(region.material);
        const float rho0 = materials_[mat].restDensity;
        // m = rho0 * s^3: each particle owns one lattice cell of fluid.
        // Deriving mass from spacing rather than reading it from config is
        // the fix for the very first runtime bug this project hit; see
        // docs/architecture.md.
        const float mass = rho0 * spacing_ * spacing_ * spacing_;

        glm::vec3 centre(0.0f);
        for (const glm::vec3& p : sites) centre += p;
        centre /= static_cast<float>(sites.size());

        for (const glm::vec3& p : sites) {
            if (particles_.size() >= maxParticles_) { hitCeiling_ = true; break; }
            Particle fp;
            fp.position = p;
            fp.velocity = region.velocityAt(p, centre);
            fp.mass = mass;
            fp.density = rho0;
            fp.material = mat;
            fp.kind = ParticleKind::Fluid;
            particles_.push_back(fp);
        }
    }

    emitterSites_.resize(scenario_.emitters.size());
    for (size_t i = 0; i < scenario_.emitters.size(); ++i) {
        // Centre-anchored: an emitter aperture is routinely thinner than
        // one particle spacing, and a corner-anchored lattice can miss it
        // entirely. See sampleVolumeCentered in scene/Shapes.hpp.
        sampleVolumeCentered(scenario_.emitters[i].shape, spacing_, emitterSites_[i]);
        if (emitterSites_[i].empty()) {
            // Loud, because the alternative is a scenario that runs
            // happily to completion having emitted nothing.
            std::cerr << "[Simulation] Emitter " << i << " in scenario '" << scenario_.name
                      << "' produced no emission sites at spacing " << spacing_
                      << " m. Its aperture is probably smaller than one particle spacing; "
                         "enlarge it or run at a finer --quality.\n";
        }
    }
}

void Simulation::updateObstacles(float t) {
    if (!anyMovingObstacle_) return;

    const int n = static_cast<int>(boundaryCount_);
    // Evaluated once per obstacle, not once per particle: translationAt
    // and velocityAt are pure functions of t, and velocityAt costs two
    // TimeSeries evaluations.
    std::vector<glm::vec3> offset(scenario_.obstacles.size());
    std::vector<glm::vec3> vel(scenario_.obstacles.size());
    for (size_t i = 0; i < scenario_.obstacles.size(); ++i) {
        offset[i] = scenario_.obstacles[i].motion.translationAt(t);
        vel[i] = scenario_.obstacles[i].motion.velocityAt(t);
    }

    #pragma omp parallel for schedule(static)
    for (int i = 0; i < n; ++i) {
        const int owner = boundaryOwner_[static_cast<size_t>(i)];
        if (owner < 0) continue;
        particles_[static_cast<size_t>(i)].position =
            boundaryRest_[static_cast<size_t>(i)] + offset[static_cast<size_t>(owner)];
        particles_[static_cast<size_t>(i)].velocity = vel[static_cast<size_t>(owner)];
    }
}

void Simulation::updateBodyAcceleration(float t) {
    glm::vec3 a = scenario_.gravity;
    for (const ExternalForce& f : scenario_.externalForces) {
        a += f.direction * f.magnitude.at(t);
    }
    bodyAcceleration_ = a;
    forceParams_.bodyAcceleration = a;
}

int Simulation::runEmitters(float dt) {
    if (scenario_.emitters.empty()) return 0;
    int emitted = 0;

    for (size_t ei = 0; ei < scenario_.emitters.size(); ++ei) {
        Emitter& e = scenario_.emitters[ei];
        if (time_ < e.startTime) continue;
        if (e.endTime >= 0.0f && time_ > e.endTime) continue;

        const float speed = e.speed * e.schedule.at(time_);
        if (speed <= 0.0f) continue;
        e.layerDebt += speed * dt;

        const std::uint8_t mat = scenario_.materialIndex(e.material);
        const float rho0 = materials_[mat].restDensity;
        const float mass = rho0 * spacing_ * spacing_ * spacing_;
        const glm::vec3 dir = glm::length(e.direction) > 0.0f
                                   ? glm::normalize(e.direction)
                                   : glm::vec3(0.0f, -1.0f, 0.0f);

        int layers = 0;
        while (e.layerDebt >= spacing_ && layers < kMaxLayersPerStep) {
            e.layerDebt -= spacing_;
            // The remaining debt is how far this layer should already have
            // travelled, so consecutive layers within one step land exactly
            // `spacing` apart instead of on top of each other.
            const glm::vec3 shift = dir * e.layerDebt;

            for (const glm::vec3& site : emitterSites_[ei]) {
                if (particles_.size() >= maxParticles_) {
                    if (!hitCeiling_) {
                        std::cerr << "[Simulation] Particle ceiling " << maxParticles_
                                  << " reached at t=" << time_
                                  << " s; emitters stopped. Raise --max-particles or "
                                     "lower the resolution if this was not intended.\n";
                    }
                    hitCeiling_ = true;
                    return emitted;
                }
                Particle p;
                p.position = site + shift;
                p.velocity = dir * speed;
                p.mass = mass;
                p.density = rho0;
                p.material = mat;
                p.kind = ParticleKind::Fluid;
                particles_.push_back(p);
                ++emitted;
            }
            ++layers;
        }
    }
    return emitted;
}

bool Simulation::insideOpenExit(const glm::vec3& p) const {
    const glm::vec3& lo = scenario_.domain.min;
    const glm::vec3& hi = scenario_.domain.max;
    const FaceMode* f = scenario_.domain.faces;
    if (f[0] == FaceMode::Open && p.x < lo.x) return true;
    if (f[1] == FaceMode::Open && p.x > hi.x) return true;
    if (f[2] == FaceMode::Open && p.y < lo.y) return true;
    if (f[3] == FaceMode::Open && p.y > hi.y) return true;
    if (f[4] == FaceMode::Open && p.z < lo.z) return true;
    if (f[5] == FaceMode::Open && p.z > hi.z) return true;
    return false;
}

int Simulation::runSinks() {
    if (!needsCompaction_) return 0;

    // STABLE, SERIAL COMPACTION. Order-preserving removal is what keeps the
    // array a pure function of the scenario and the step count: a parallel
    // scatter would produce a different surviving order per thread count,
    // and every later neighbour sum would then be a differently-ordered
    // float sum. It is O(N) with trivial per-particle work, alongside
    // O(N * neighbours) loops -- not worth the risk to thread.
    const size_t begin = boundaryCount_;
    size_t write = begin;
    int removed = 0;

    for (size_t read = begin; read < particles_.size(); ++read) {
        const Particle& p = particles_[read];
        bool drop = insideOpenExit(p.position);
        if (!drop) {
            for (const Sink& s : scenario_.sinks) {
                if (time_ < s.startTime) continue;
                if (s.endTime >= 0.0f && time_ > s.endTime) continue;
                if (s.shape.contains(p.position)) { drop = true; break; }
            }
        }
        if (drop) { ++removed; continue; }
        if (write != read) particles_[write] = particles_[read];
        ++write;
    }

    if (removed > 0) particles_.resize(write);
    return removed;
}

const StepStats& Simulation::step() {
    updateObstacles(time_);
    updateBodyAcceleration(time_);

    grid_->build(particles_);
    if (anyMovingObstacle_) {
        // Rigid translation does not change distances *within* one
        // obstacle, but it does change them between a moving paddle and
        // the tank walls it slides past, so the volumes are refreshed
        // while anything is moving. Static scenarios never pay this.
        computeBoundaryVolumes(particles_, *grid_, *kernel_, static_cast<int>(boundaryCount_));
    }

    const int firstFluid = static_cast<int>(boundaryCount_);
    computeDensityPressure(particles_, *grid_, *kernel_, eosTable_, restDensities_, firstFluid);
    computeSurfaceNormals(particles_, *grid_, *kernel_, materials_, firstFluid);
    computeForces(particles_, *grid_, *kernel_, forceParams_);

    const TimeStepInfo ts = timestep_->compute(particles_);

    const auto recompute = [&](std::vector<Particle>& p) {
        computeForces(p, *grid_, *kernel_, forceParams_);
    };
    integrator_->step(particles_, ts.dt, recompute);

    time_ += ts.dt;
    ++stepCount_;
    prevDt_ = ts.dt;

    const int removed = runSinks();
    const int emitted = runEmitters(ts.dt);

    integrator_->resetOutflow();
    refreshStats(ts, emitted, removed);
    return stats_;
}

void Simulation::refreshStats(const TimeStepInfo& ts, int emitted, int removed) {
    const int n = static_cast<int>(particles_.size());
    const int b = static_cast<int>(boundaryCount_);
    const int fluid = n - b;

    stats_.step = stepCount_;
    stats_.time = time_;
    stats_.dt = ts.dt;
    stats_.dtCfl = ts.dtCfl;
    stats_.dtForce = ts.dtForce;
    stats_.dtViscous = ts.dtViscous;
    stats_.maxSpeed = ts.maxSpeed;
    stats_.maxAccel = ts.maxAccel;
    stats_.fluidCount = fluid;
    stats_.boundaryCount = b;
    stats_.emittedThisStep = emitted;
    stats_.removedThisStep = removed;
    stats_.containmentEvents = integrator_->containmentEvents();

    if (fluid <= 0) {
        stats_.densityMin = stats_.densityMax = stats_.densityAvg = 0.0f;
        stats_.fractionNearRest = 0.0f;
        stats_.fluidVolume = 0.0;
        return;
    }

    // Deterministic reductions throughout: these numbers are asserted on
    // by CI and quoted in docs, so they must not wobble with thread count.
    stats_.densityMin = -reduce::deterministicMax(fluid, -1.0e30f, [&](int i) {
        return -particles_[static_cast<size_t>(b + i)].density;
    });
    stats_.densityMax = reduce::deterministicMax(fluid, 0.0f, [&](int i) {
        return particles_[static_cast<size_t>(b + i)].density;
    });
    stats_.densityAvg = static_cast<float>(
        reduce::deterministicSum(fluid, [&](int i) {
            return particles_[static_cast<size_t>(b + i)].density;
        }) / fluid);

    const long long near = reduce::deterministicCount(fluid, [&](int i) {
        const Particle& p = particles_[static_cast<size_t>(b + i)];
        const float rho0 = restDensities_[std::min<size_t>(p.material, restDensities_.size() - 1)];
        return std::abs(p.density - rho0) <= 0.01f * rho0;
    });
    stats_.fractionNearRest = static_cast<float>(near) / static_cast<float>(fluid);

    stats_.fluidVolume = reduce::deterministicSum(fluid, [&](int i) {
        const Particle& p = particles_[static_cast<size_t>(b + i)];
        return p.density > 0.0f ? p.mass / p.density : 0.0f;
    });
}

int Simulation::unstableCount() const {
    const int n = static_cast<int>(particles_.size());
    const int b = static_cast<int>(boundaryCount_);
    const glm::vec3& lo = scenario_.domain.min;
    const glm::vec3& hi = scenario_.domain.max;
    const FaceMode* f = scenario_.domain.faces;
    // Half a particle spacing, not a fixed millimetre: fluid resting
    // against a boundary genuinely sits within half a spacing of the
    // nominal domain plane (the no-penetration surface is midway to the
    // first boundary layer), so a fixed absolute tolerance either flags
    // settled fluid as unstable at fine resolution or misses real escapes
    // at coarse resolution. This scales with the discretisation the same
    // way the containment tolerance does, and is deliberately the looser
    // of the two so containment fires before instability is declared.
    const float tol = 0.5f * spacing_;

    return static_cast<int>(reduce::deterministicCount(n - b, [&](int i) {
        const glm::vec3& x = particles_[static_cast<size_t>(b + i)].position;
        if (!std::isfinite(x.x) || !std::isfinite(x.y) || !std::isfinite(x.z)) return true;
        // An Open face is an exit, not a containment failure. Counting
        // departures through one as instability would mark every flood,
        // channel and river scenario UNSTABLE by construction.
        if (f[0] != FaceMode::Open && x.x < lo.x - tol) return true;
        if (f[1] != FaceMode::Open && x.x > hi.x + tol) return true;
        if (f[2] != FaceMode::Open && x.y < lo.y - tol) return true;
        if (f[3] != FaceMode::Open && x.y > hi.y + tol) return true;
        if (f[4] != FaceMode::Open && x.z < lo.z - tol) return true;
        if (f[5] != FaceMode::Open && x.z > hi.z + tol) return true;
        return false;
    }));
}

bool Simulation::finished() const {
    return time_ >= scenario_.duration.simulatedTime ||
           stepCount_ >= scenario_.duration.maxSteps;
}

} // namespace aquasph
