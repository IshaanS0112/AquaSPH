#pragma once
#include <string>
#include <glm/glm.hpp>

namespace aquasph {

struct Config {
    // --- Particle / kernel ---
    int   particleCount  = 5000;    // target; the dam-break block's actual count is derived from spacing + block geometry
    float h              = 0.1f;    // smoothing radius (m); kernel support radius
    float mass           = 1.0f;    // per-particle mass (kg), uniform

    // --- Fluid (Tait EOS, water) ---
    float restDensity    = 1000.0f; // rho0, kg/m^3
    float soundSpeed     = 40.0f;   // c0, m/s -- artificial, NOT water's true ~1480 m/s; see docs/architecture.md
    float gamma           = 7.0f;
    // mu, dynamic viscosity in Pa*s within the Morris et al. (1997)
    // discretisation. RETUNED IN V2: the v1 value belonged to a term that
    // was both wrongly signed and effectively divided by particle mass, so
    // its magnitude carried no physical meaning. See docs/architecture.md.
    float viscosity        = 5.0f;

    // --- Time integration ---
    float dt              = 0.001f;  // s -- now the *upper bound* on the adaptive step (see core/TimeStep.hpp)
    int   maxSteps         = 1000;
    int   reportInterval   = 100;

    // --- Domain / gravity ---
    glm::vec3 gravity    {0.0f, -9.81f, 0.0f};
    glm::vec3 domainMin  {-1.0f, -1.0f, -1.0f};
    glm::vec3 domainMax  { 1.0f,  1.0f,  1.0f};

    // Fraction of wall-normal velocity kept (and reversed) on a boundary
    // hit. The spec calls for a simple "flip velocity component" wall
    // response, which is a *bounce*. That's fine for a rigid body, but
    // for a fluid it's physically wrong -- a water dam-break shouldn't
    // ricochet off the floor -- and empirically it's actively dangerous
    // here: the whole bottom face of the initial block reaches the floor
    // within the same few timesteps, and reflecting that many
    // simultaneous impacts back into the still-falling particles above
    // triggers a compression cascade (observed: density running away
    // within ~100 steps of floor contact). A small value close to 0
    // approximates an inelastic/absorbing wall, which is the right
    // default for a free-surface liquid; see docs/architecture.md.
    float wallDamping = 0.05f;

    // Monaghan (1989) XSPH coefficient. See core/ForceCompute.cpp.
    float xsphEpsilon = 0.5f;

    // RENDER-ONLY. The speed (m/s) that maps to the top of the colour
    // ramp. v1 reused the integrator's velocity clamp for this, which
    // meant most particles sat at exactly the clamp value during the
    // interesting part of the run and rendered as a flat white sheet.
    // Visualisation normalisation is now a separate, per-scenario number
    // and never touches the physics.
    float referenceSpeed = 4.0f;
};

class ConfigLoader {
public:
    // Loads a JSON config from `path`. Falls back to built-in defaults
    // (with a warning on stderr) if the file is missing or malformed --
    // The solver should never hard-crash just because a config path is wrong.
    static Config load(const std::string& path);
};

} // namespace aquasph
