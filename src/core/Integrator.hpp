#pragma once
#include <vector>
#include <functional>
#include <glm/glm.hpp>
#include "Particle.hpp"

namespace aquasph {

// What happens at a domain face.
enum class FaceMode {
    // Containment: the particle is placed back on the face and its outward-normal velocity
    // component is reversed and scaled by `damping`.
    Solid,
    // Open: the particle has left the simulated region.
    Open,

    // THERE IS NO Periodic MODE, and its absence is deliberate.
};

struct BoundaryBox {
    glm::vec3 min;
    glm::vec3 max;
    // Velocity retained (and reversed) on a containment hit.
    // 0 = inelastic/absorbing, 1 = perfectly elastic.
    float damping = 0.5f;

    // Containment does not fire until a particle is this far outside the face.
    float tolerance = 0.0f;
    // Per-face behaviour: -x, +x, -y, +y, -z, +z.
    FaceMode faces[6] = {FaceMode::Solid, FaceMode::Solid, FaceMode::Solid,
                          FaceMode::Solid, FaceMode::Solid, FaceMode::Solid};
};

// 2nd-order predictor-corrector integrator: The force-recompute step is injected as a callback
// rather than the integrator depending on ForceCompute/LinkedCell directly.
class PredictorCorrectorIntegrator {
public:
    explicit PredictorCorrectorIntegrator(const BoundaryBox& bounds);

    void step(std::vector<Particle>& particles, float dt,
              const std::function<void(std::vector<Particle>&)>& recomputeForces);

    // Number of times a Solid face had to push a particle back in-bounds since the last
    // resetContainmentEvents().
    long long containmentEvents() const { return containmentEvents_; }
    void resetContainmentEvents() { containmentEvents_ = 0; }

    // True once a particle has crossed an Open face and is waiting to be
    // removed by the owner. Cleared by resetOutflow().
    bool hasOutflow() const { return outflow_; }
    void resetOutflow() { outflow_ = false; }

    const BoundaryBox& bounds() const { return bounds_; }

    // Index of the first fluid particle; the integration loops start there instead of walking a
    // long prefix of boundary particles they would only skip.
    void setFirstFluidIndex(int i) { firstFluid_ = i; }

private:
    BoundaryBox bounds_;
    int firstFluid_ = 0;

    // Scratch for the predictor-corrector's saved initial velocity and force.
    std::vector<glm::vec3> v0_;
    std::vector<glm::vec3> f0_;
    long long containmentEvents_ = 0;
    bool outflow_ = false;

    void applyBoundary(Particle& p, long long& events, bool& outflow) const;
};

} // namespace aquasph
