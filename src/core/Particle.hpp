#pragma once
#include <cstdint>
#include <glm/glm.hpp>

namespace aquasph {

// What a particle *is*, which decides how the solver treats it.
//
// Boundary particles live in the SAME array as fluid particles, on
// purpose: LinkedCell then finds them through the identical 27-cell
// stencil the fluid already uses, so obstacles of arbitrary static shape
// cost the neighbour search nothing extra and need no second data
// structure. They are never integrated -- their position is prescribed,
// not solved for. See core/BoundaryVolume.hpp for how they contribute to
// density and pressure (Akinci et al. 2012).
enum class ParticleKind : std::uint8_t {
    Fluid = 0,
    Boundary = 1,
};

// AoS (Array-of-Structures) layout: each Particle owns all of its state
// contiguously.
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
// Correctness came first here, so AoS is used. Revisit if profiling shows
// the density/force loops are memory-bandwidth bound rather than
// neighbor-search or compute bound.
//
// GROWTH NOTE (v2): `normal` and `xsphDelta` are per-step scratch, not
// persistent state -- they could live in separate parallel arrays owned
// by Simulation instead, keeping this struct at its original 48 bytes.
// They are kept here because both are read one step later by code that
// already has the Particle in hand (the integrator needs xsphDelta; the
// surface-tension term needs the neighbour's normal), so hoisting them
// out would mean threading two more spans through every signature for a
// 24-byte saving. Measured cost of the growth is reported in
// benchmarks/scaling_results.md.
struct Particle {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 force{0.0f};

    // Colour-field normal used by the Akinci et al. (2013) surface-tension
    // curvature term. Recomputed every step; meaningless for boundary
    // particles.
    glm::vec3 normal{0.0f};

    // XSPH velocity correction (Monaghan 1989): the neighbourhood-averaged
    // velocity offset used to *advect* the particle, without altering the
    // momentum-carrying velocity. Recomputed every step.
    glm::vec3 xsphDelta{0.0f};

    float density = 0.0f;
    float pressure = 0.0f;

    // Fluid particles: real mass, kg (derived from lattice spacing and the
    // material's rest density -- see FluidRegion.cpp).
    // Boundary particles: unused; see `volume` instead.
    float mass = 1.0f;

    // Boundary particles only: the Akinci et al. (2012) boundary volume
    // V_b = 1 / sum_k W(r_bk) over neighbouring *boundary* particles. Its
    // contribution to a fluid particle i is the pseudo-mass
    // Psi_b = rho0(material of i) * V_b, which makes the boundary's
    // effect independent of how densely it happens to be sampled.
    float volume = 0.0f;

    ParticleKind kind = ParticleKind::Fluid;

    // Index into the scenario's MaterialTable. Boundary particles carry
    // 0; they have no material of their own.
    std::uint8_t material = 0;
};

inline bool isFluid(const Particle& p) { return p.kind == ParticleKind::Fluid; }
inline bool isBoundary(const Particle& p) { return p.kind == ParticleKind::Boundary; }

} // namespace aquasph
