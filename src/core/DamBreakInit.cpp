#include "DamBreakInit.hpp"
#include <cmath>
#include <algorithm>

namespace aquasph {

// PARTICLE MASS IS DERIVED, NOT READ FROM CONFIG. A very common SPH bug
// (caught here by actually running the sim and looking at reported
// density -- see docs/architecture.md) is initializing every particle
// with an arbitrary fixed mass, disconnected from the lattice spacing.
// SPH density is mass-weighted: rho_i = sum_j m_j * W(r_ij, h). For a
// uniform lattice at spacing s to sum to rho0, each particle must own
// approximately one "cell" of fluid, i.e. m = rho0 * s^3. Using
// cfg.mass=1.0 directly (the spec's literal "uniform, normalized" value)
// with h=0.1/spacing=0.05 initialized densities at ~7.6x rest density
// before the simulation even took a single step -- Tait pressure scales
// with (rho/rho0)^gamma, so an 7.6x density error blew up into a
// pressure error of roughly 7.6^7 (~2 million x), which is exactly the
// runaway seen in early testing. cfg.mass is kept in Config for
// documentation/reference (and is what a *non-lattice* initializer, e.g.
// a particle emitter, would use directly), but the dam-break block
// computes its own physically-consistent mass here.
//
// SPACING IS DERIVED FROM THE TARGET PARTICLE COUNT, not hardcoded to
// h/2. This matters for the benchmark harness (scripts/run wants to
// sweep particle count -- 5k/10k/25k/50k -- to measure how per-step cost
// scales with N): a fixed spacing would mean a fixed particle count, so
// "more particles" would have nowhere to go. Solving blockVolume/spacing^3
// = targetCount for spacing keeps the block's physical footprint fixed
// and just packs it more or less densely. h/2 (matching cfg.h, the
// default target of ~6859) is still the *design point* spacing this
// project is tuned around (see docs/architecture.md for the stability
// tuning); denser benchmark runs are for throughput measurement, not
// physical-accuracy validation.
std::vector<Particle> initializeDamBreak(const Config& cfg, int targetCount) {
    const glm::vec3 blockMin = cfg.domainMin + glm::vec3(0.02f);
    const glm::vec3 blockSize(0.9f, 0.9f, 0.9f);
    const float blockVolume = blockSize.x * blockSize.y * blockSize.z;

    const float spacing = std::cbrt(blockVolume / static_cast<float>(std::max(1, targetCount)));
    const float mass = cfg.restDensity * spacing * spacing * spacing;

    const int nx = static_cast<int>(blockSize.x / spacing) + 1;
    const int ny = static_cast<int>(blockSize.y / spacing) + 1;
    const int nz = static_cast<int>(blockSize.z / spacing) + 1;

    std::vector<Particle> particles;
    particles.reserve(static_cast<size_t>(nx) * ny * nz);

    for (int ix = 0; ix < nx; ++ix) {
        for (int iy = 0; iy < ny; ++iy) {
            for (int iz = 0; iz < nz; ++iz) {
                Particle p;
                p.position = blockMin + glm::vec3(static_cast<float>(ix),
                                                    static_cast<float>(iy),
                                                    static_cast<float>(iz)) * spacing;
                p.velocity = glm::vec3(0.0f);
                p.force = glm::vec3(0.0f);
                p.mass = mass;
                p.density = cfg.restDensity;
                p.pressure = 0.0f;
                particles.push_back(p);
            }
        }
    }
    return particles;
}

} // namespace aquasph
