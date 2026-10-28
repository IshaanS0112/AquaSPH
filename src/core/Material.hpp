#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "DensityPressure.hpp"

namespace aquasph {

// A fluid's physical parameters, gathered in one place so a scenario can say "this region is
// water, that jet is a lighter fluid" in JSON without any solver change.
struct Material {
    std::string name = "water";

    // rho0, kg/m^3.
    float restDensity = 1000.0f;

    // Artificial speed of sound, m/s.
    float soundSpeed = 40.0f;

    // Tait exponent.
    float gamma = 7.0f;

    // Dynamic viscosity, Pa*s, as it enters the Morris et al. (1997) viscous-Laplacian
    // discretisation in ForceCompute.cpp.
    float viscosity = 5.0f;

    // Akinci et al. (2013) surface-tension coefficient.
    float surfaceTension = 0.0f;

    // Render-only, inert to the solver: beer-Lambert
    // absorption coefficients (1/m, per RGB channel) used by the screen-space renderer's
    // thickness compositing.
    glm::vec3 absorption{0.55f, 0.16f, 0.09f};

    TaitEOS eos() const { return TaitEOS(restDensity, soundSpeed, gamma); }
};

using MaterialTable = std::vector<Material>;

// Precomputed Tait constants, one per material, so the density loop does
// not reconstruct a TaitEOS per particle.
inline std::vector<TaitEOS> makeEosTable(const MaterialTable& materials) {
    std::vector<TaitEOS> table;
    table.reserve(materials.size());
    for (const Material& m : materials) table.push_back(m.eos());
    return table;
}

} // namespace aquasph
