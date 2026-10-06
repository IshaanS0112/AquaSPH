#pragma once
#include <cstdint>
#include <glm/glm.hpp>

namespace aquasph {

// What a particle *is*, which decides how the solver treats it.
enum class ParticleKind : std::uint8_t {
    Fluid = 0,
    Boundary = 1,
};

// AoS (Array-of-Structures) layout: each Particle owns all of its state contiguously.
struct Particle {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 force{0.0f};

    // Colour-field normal used by the Akinci et al. (2013) surface-tension curvature term.
    glm::vec3 normal{0.0f};

    // XSPH velocity correction (Monaghan 1989): the neighbourhood-averaged velocity offset used
    // to *advect* the particle, without altering the momentum-carrying velocity.
    glm::vec3 xsphDelta{0.0f};

    float density = 0.0f;
    float pressure = 0.0f;

    // Fluid particles: real mass, kg (derived from lattice spacing and the material's rest
    // density -- see FluidRegion.cpp).
    float mass = 1.0f;

    // Boundary particles only: the Akinci et al. (2012) boundary volume V_b = 1 / sum_k W(r_bk)
    // over neighbouring *boundary* particles.
    float volume = 0.0f;

    ParticleKind kind = ParticleKind::Fluid;

    // Index into the scenario's MaterialTable. Boundary particles carry
    // 0; they have no material of their own.
    std::uint8_t material = 0;

    // Explicit zeroed padding: implicit padding is uninitialised under Clang, which made
    // memcmp-based determinism checks fail on macOS even though every field matched.
    std::uint8_t reserved[2] = {0, 0};
};

static_assert(sizeof(Particle) == 5 * sizeof(glm::vec3) + 4 * sizeof(float) + 4,
              "Particle must have no implicit padding bytes");

inline bool isFluid(const Particle& p) { return p.kind == ParticleKind::Fluid; }
inline bool isBoundary(const Particle& p) { return p.kind == ParticleKind::Boundary; }

} // namespace aquasph
