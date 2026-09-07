#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "DensityPressure.hpp"

namespace aquasph {

// A fluid's physical parameters, gathered in one place so a scenario can
// say "this region is water, that jet is a lighter fluid" in JSON without
// any solver change. Before this existed, rest density / sound speed /
// gamma / viscosity were global Config fields, which made "a dam break
// wants far less surface tension than a droplet impact" impossible to
// express without editing C++.
//
// SCOPE, STATED PLAINLY: multiple Material entries can coexist in one
// scenario and each particle carries its own material index, so a
// two-fluid setup will *run*. That is not multiphase physics. The solver
// uses a single-phase weakly-compressible SPH discretisation throughout:
// there is no interface tension between different materials, no density
// -ratio-stable pressure formulation, and no phase-change or mixing
// model. At a large density ratio the interface will be numerically
// dominated by the standard SPH artefacts. Multiphase flow is a Tier 3
// extension point (docs/architecture.md), not a feature of this solver.
struct Material {
    std::string name = "water";

    // rho0, kg/m^3.
    float restDensity = 1000.0f;

    // Artificial speed of sound, m/s. NOT water's true ~1480 m/s: weakly
    // -compressible SPH picks c0 just large enough to hold density
    // fluctuations near 1% while keeping the CFL timestep affordable.
    // A useful rule is c0 >= 10 * v_max_expected.
    float soundSpeed = 40.0f;

    // Tait exponent.
    float gamma = 7.0f;

    // Dynamic viscosity, Pa*s, as it enters the Morris et al. (1997)
    // viscous-Laplacian discretisation in ForceCompute.cpp.
    //
    // These are ARTIFICIAL values, far above water's physical 1.0e-3
    // Pa*s, and deliberately so. At h = 0.02-0.1 m the simulation cannot
    // resolve the turbulent cascade that dissipates energy in a real
    // dam break; without a numerically enlarged viscosity that energy
    // has nowhere to go and reappears as particle-scale noise. Treat
    // this as a sub-particle dissipation model, not a fluid property,
    // and do not read Reynolds numbers off it.
    float viscosity = 5.0f;

    // Akinci et al. (2013) surface-tension coefficient. Per-material and
    // per-scenario on purpose: a droplet-impact crown needs a strong
    // term, a metre-scale dam break needs essentially none (its Weber
    // number is enormous, so surface tension is physically irrelevant
    // there and only costs time).
    float surfaceTension = 0.0f;

    // --- Render-only, inert to the solver -------------------------------
    // Beer-Lambert absorption coefficients (1/m, per RGB channel) used by
    // the screen-space renderer's thickness compositing. Kept on Material
    // because it is a property *of the fluid* and belongs wherever rest
    // density does; the solver never reads it, and nothing in src/core or
    // src/scene branches on it.
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
