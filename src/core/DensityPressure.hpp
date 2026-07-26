#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include "Particle.hpp"
#include "SPHKernel.hpp"

namespace aquasph {

class LinkedCell;

// Weakly-compressible Tait equation of state:
//   P = B * ((rho/rho0)^gamma - 1)
//   B = c0^2 * rho0 / gamma
//
// c0 is an *artificial* speed of sound, not necessarily water's true
// ~1480 m/s. In weakly-compressible SPH, c0 is deliberately chosen large
// enough that density fluctuations stay small (~1%) -- which is what
// keeps the simulation close to incompressible -- but small enough that
// the CFL time-step restriction (dt <~ 0.4*h/c0) doesn't force an
// impractically tiny dt. See docs/architecture.md for how this project's
// default c0 was chosen (the literal 1400 m/s + dt=0.001s combination
// from the original spec violates CFL by >30x and was retuned).
//
// PRESSURE IS CLAMPED TO BE NON-NEGATIVE. This is a deliberate deviation
// from the textbook Tait formula, found necessary by actually running the
// dam-break scenario: free-surface particles (any particle near the
// block's outer boundary, which for a small block is a large fraction of
// them) register slightly *below* rest density at initialization purely
// from having an incomplete kernel neighborhood -- there's no fluid
// beyond the surface to contribute to their density sum. That makes the
// raw Tait formula return P < 0 ("tensile" pressure) for those particles.
// Because the SPH pressure-gradient force is symmetric in P_i + P_j,
// negative pressure flips it from repulsive to *attractive* -- this is
// the well-documented SPH "tensile instability": surface particles
// spuriously clump, which raises their local density further, which
// (given P ~ (rho/rho0)^7) produces a wildly oversensitive restoring
// force once density crosses back above rho0, and the whole thing
// runs away (observed directly: reported average density went from
// ~950 to ~900,000+ within 15 timesteps before this fix). Clamping
// P = max(0, P_tait) is the standard, pragmatic mitigation used in many
// production weakly-compressible SPH codes for exactly this free-surface
// case; it sacrifices modeling true tensile/cavitation stresses (not a
// concern for a dam-break free-surface flow) in exchange for stability.
struct TaitEOS {
    float restDensity;
    float soundSpeed;
    float gamma;
    float B;

    TaitEOS(float restDensity_, float soundSpeed_, float gamma_)
        : restDensity(restDensity_), soundSpeed(soundSpeed_), gamma(gamma_) {
        B = (soundSpeed * soundSpeed * restDensity) / gamma;
    }

    float pressure(float density) const {
        const float ratio = density / restDensity;
        const float p = B * (std::pow(ratio, gamma) - 1.0f);
        return std::max(0.0f, p);
    }
};

// For every particle: rho_i = sum_j m_j * W(|r_i - r_j|, h) (including the
// particle's own self-contribution at r=0, which is standard SPH
// practice), then pressure_i = TaitEOS::pressure(rho_i). Uses the
// linked-cell grid to restrict the neighbor search to the particle's own
// cell plus its 26 adjacent cells instead of an O(N^2) all-pairs scan.
void computeDensityPressure(std::vector<Particle>& particles,
                             const LinkedCell& grid,
                             const CubicSplineKernel& kernel,
                             const TaitEOS& eos);

} // namespace aquasph
