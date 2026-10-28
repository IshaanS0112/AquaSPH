#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include "Particle.hpp"
#include "SPHKernel.hpp"

namespace aquasph {

class LinkedCell;

// Weakly-compressible Tait equation of state: c0 is an *artificial* speed of sound, not
// necessarily water's true ~1480 m/s.
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

// For every particle: rho_i = sum_j m_j * W(|r_i - r_j|, h) (including the particle's own
// self-contribution at r=0, which is standard SPH practice), then pressure_i =
// TaitEOS::pressure(rho_i).
void computeDensityPressure(std::vector<Particle>& particles,
                             const LinkedCell& grid,
                             const CubicSplineKernel& kernel,
                             const TaitEOS& eos);

// MULTI-MATERIAL / BOUNDARY-AWARE OVERLOAD, used by the scenario engine.
void computeDensityPressure(std::vector<Particle>& particles,
                             const LinkedCell& grid,
                             const CubicSplineKernel& kernel,
                             const std::vector<TaitEOS>& eosByMaterial,
                             const std::vector<float>& restDensityByMaterial,
                             int firstFluid = 0);

} // namespace aquasph
