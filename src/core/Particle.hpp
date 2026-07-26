#pragma once
#include <glm/glm.hpp>

namespace aquasph {

// AoS (Array-of-Structures) layout: each Particle owns all of its state
// contiguously. This is what Phase 0 uses.
//
// Tradeoff vs SoA (Structure-of-Arrays -- separate std::vector<vec3> for
// positions, std::vector<float> for densities, etc.): SoA is generally
// more cache- and SIMD-friendly for the density/force loops, since a loop
// that only touches position and density doesn't have to pull mass,
// pressure, and force into cache alongside it the way AoS does (they all
// live in the same struct). SoA also auto-vectorizes more readily.
// The cost is more invasive code: every "particle" access becomes several
// parallel-array index operations instead of one struct dereference,
// which slows down development and obscures the physics.
//
// Phase 0 is single-threaded and correctness-first (see project notes:
// "no premature optimization -- get it working first, parallelize in
// Phase 1"), so AoS is used here. Revisit in Phase 1 if profiling shows
// the density/force loops are memory-bandwidth bound rather than
// neighbor-search or compute bound.
struct Particle {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 force{0.0f};
    float density = 0.0f;
    float pressure = 0.0f;
    float mass = 1.0f;
};

} // namespace aquasph
