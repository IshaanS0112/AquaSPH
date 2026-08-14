#include "Integrator.hpp"

namespace aquasph {

namespace {
glm::vec3 clampSpeed(const glm::vec3& v, float maxSpeed) {
    const float speed = glm::length(v);
    if (speed > maxSpeed && speed > 0.0f) {
        return v * (maxSpeed / speed);
    }
    return v;
}
} // namespace

PredictorCorrectorIntegrator::PredictorCorrectorIntegrator(const BoundaryBox& bounds, float maxSpeed)
    : bounds_(bounds), maxSpeed_(maxSpeed) {}

void PredictorCorrectorIntegrator::step(
    std::vector<Particle>& particles, float dt,
    const std::function<void(std::vector<Particle>&)>& recomputeForces) {

    const int n = static_cast<int>(particles.size());
    std::vector<glm::vec3> v0(n), f0(n);

    // All three loops below touch only particles[i] for their own index
    // i -- no cross-particle reads or writes -- so each parallelizes
    // trivially. Lighter per-iteration than the density/force
    // kernels (no neighbor search here), but still real work at the
    // particle counts this project targets, so still worth threading.
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < n; ++i) {
        v0[i] = particles[i].velocity;
        f0[i] = particles[i].force;
    }

    // Step 1: predicted half-step velocity, written into particle.velocity
    // so the recompute callback's viscosity term sees it. Clamped for the
    // same reason as the final velocity below (see header comment) --
    // otherwise a single huge f0 can already poison the viscosity
    // recompute in step 2 before the final clamp gets a chance to act.
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < n; ++i) {
        const glm::vec3 vHalf = v0[i] + (f0[i] / particles[i].mass) * (dt * 0.5f);
        particles[i].velocity = clampSpeed(vHalf, maxSpeed_);
    }

    // Step 2: re-evaluate forces at the half-step velocity. computeForces
    // is itself parallelized internally; this call sits between two
    // separate parallel regions rather than nesting inside one, since
    // the `#pragma omp parallel for` above has already joined all
    // threads back to the caller by the time this runs.
    recomputeForces(particles);

    // Step 3 & 4: correct velocity using the half-step force, advance
    // position with the corrected velocity, then apply boundaries.
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < n; ++i) {
        Particle& p = particles[i];
        const glm::vec3 vNew = clampSpeed(v0[i] + (p.force / p.mass) * dt, maxSpeed_);
        const glm::vec3 posNew = p.position + vNew * dt;

        p.velocity = vNew;
        p.position = posNew;
        applyBoundary(p);
    }
}

void PredictorCorrectorIntegrator::applyBoundary(Particle& p) const {
    auto reflect = [&](float& pos, float& vel, float lo, float hi) {
        if (pos < lo) {
            pos = lo;
            vel = -vel * bounds_.damping;
        } else if (pos > hi) {
            pos = hi;
            vel = -vel * bounds_.damping;
        }
    };
    reflect(p.position.x, p.velocity.x, bounds_.min.x, bounds_.max.x);
    reflect(p.position.y, p.velocity.y, bounds_.min.y, bounds_.max.y);
    reflect(p.position.z, p.velocity.z, bounds_.min.z, bounds_.max.z);
}

} // namespace aquasph
