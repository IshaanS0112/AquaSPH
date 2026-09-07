#include "Integrator.hpp"
#include "ParallelReduce.hpp"
#include <algorithm>

namespace aquasph {

PredictorCorrectorIntegrator::PredictorCorrectorIntegrator(const BoundaryBox& bounds)
    : bounds_(bounds) {}

void PredictorCorrectorIntegrator::step(
    std::vector<Particle>& particles, float dt,
    const std::function<void(std::vector<Particle>&)>& recomputeForces) {

    const int n = static_cast<int>(particles.size());
    const int begin = std::max(0, std::min(firstFluid_, n));
    std::vector<glm::vec3> v0(n), f0(n);

    // All three loops below touch only particles[i] for their own index
    // i -- no cross-particle reads or writes -- so each parallelizes
    // trivially. Lighter per-iteration than the density/force
    // kernels (no neighbor search here), but still real work at the
    // particle counts this project targets, so still worth threading.
    //
    // Boundary particles are carried through untouched: their position and
    // velocity are prescribed by the scenario's obstacle motion, not
    // solved for here.
    #pragma omp parallel for schedule(static)
    for (int i = begin; i < n; ++i) {
        v0[i] = particles[i].velocity;
        f0[i] = particles[i].force;
    }

    // Step 1: predicted half-step velocity, written into particle.velocity
    // so the recompute callback's viscosity term sees it.
    #pragma omp parallel for schedule(static)
    for (int i = begin; i < n; ++i) {
        if (particles[i].kind != ParticleKind::Fluid) continue;
        particles[i].velocity = v0[i] + (f0[i] / particles[i].mass) * (dt * 0.5f);
    }

    // Step 2: re-evaluate forces at the half-step velocity. computeForces
    // is itself parallelized internally; this call sits between two
    // separate parallel regions rather than nesting inside one, since
    // the `#pragma omp parallel for` above has already joined all
    // threads back to the caller by the time this runs.
    recomputeForces(particles);

    // Step 3 & 4: correct velocity using the half-step force, advance
    // position with the XSPH-corrected velocity, then apply the domain
    // faces.
    //
    // The containment counter and the outflow flag are the only
    // cross-iteration state here. They are accumulated per-thread and
    // folded once at the end rather than written under a critical
    // section: a counter is exact under any summation order, and a
    // boolean OR is order-independent, so neither can perturb the
    // determinism guarantee.
    long long events = 0;
    bool outflow = false;

    #pragma omp parallel
    {
        long long localEvents = 0;
        bool localOutflow = false;

        #pragma omp for schedule(static) nowait
        for (int i = begin; i < n; ++i) {
            Particle& p = particles[i];
            if (p.kind != ParticleKind::Fluid) continue;
            const glm::vec3 vNew = v0[i] + (p.force / p.mass) * dt;
            p.velocity = vNew;
            p.position += (vNew + p.xsphDelta) * dt;
            applyBoundary(p, localEvents, localOutflow);
        }

        #pragma omp critical
        {
            events += localEvents;
            outflow = outflow || localOutflow;
        }
    }

    containmentEvents_ += events;
    outflow_ = outflow_ || outflow;
}

void PredictorCorrectorIntegrator::applyBoundary(Particle& p, long long& events,
                                                  bool& outflow) const {
    const float tol = bounds_.tolerance;
    auto handle = [&](float& pos, float& vel, float lo, float hi,
                       FaceMode loMode, FaceMode hiMode) {
        const float span = hi - lo;
        if (pos < lo - tol) {
            switch (loMode) {
                case FaceMode::Solid:
                    pos = lo;
                    vel = -vel * bounds_.damping;
                    ++events;
                    break;
                case FaceMode::Open:
                    outflow = true;
                    break;
                case FaceMode::Periodic:
                    if (span > 0.0f) pos += span;
                    break;
            }
        } else if (pos > hi + tol) {
            switch (hiMode) {
                case FaceMode::Solid:
                    pos = hi;
                    vel = -vel * bounds_.damping;
                    ++events;
                    break;
                case FaceMode::Open:
                    outflow = true;
                    break;
                case FaceMode::Periodic:
                    if (span > 0.0f) pos -= span;
                    break;
            }
        }
    };
    handle(p.position.x, p.velocity.x, bounds_.min.x, bounds_.max.x,
           bounds_.faces[0], bounds_.faces[1]);
    handle(p.position.y, p.velocity.y, bounds_.min.y, bounds_.max.y,
           bounds_.faces[2], bounds_.faces[3]);
    handle(p.position.z, p.velocity.z, bounds_.min.z, bounds_.max.z,
           bounds_.faces[4], bounds_.faces[5]);
}

} // namespace aquasph
