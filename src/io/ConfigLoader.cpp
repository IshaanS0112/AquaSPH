#include "ConfigLoader.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

namespace aquasph {

namespace {
glm::vec3 readVec3(const nlohmann::json& j, const glm::vec3& def) {
    if (!j.is_array() || j.size() != 3) return def;
    return glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}
} // namespace

Config ConfigLoader::load(const std::string& path) {
    Config cfg;
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[ConfigLoader] Warning: could not open '" << path
                  << "', using built-in defaults.\n";
        return cfg;
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const std::exception& e) {
        std::cerr << "[ConfigLoader] Warning: failed to parse '" << path
                  << "' (" << e.what() << "), using built-in defaults.\n";
        return cfg;
    }

    cfg.particleCount  = j.value("particle_count", cfg.particleCount);
    cfg.h              = j.value("h", cfg.h);
    cfg.mass           = j.value("mass", cfg.mass);
    cfg.restDensity    = j.value("rest_density", cfg.restDensity);
    cfg.soundSpeed     = j.value("sound_speed", cfg.soundSpeed);
    cfg.gamma          = j.value("gamma", cfg.gamma);
    cfg.viscosity      = j.value("viscosity", cfg.viscosity);
    cfg.dt             = j.value("dt", cfg.dt);
    cfg.maxSteps       = j.value("max_steps", cfg.maxSteps);
    cfg.reportInterval = j.value("report_interval", cfg.reportInterval);
    cfg.wallDamping    = j.value("wall_damping", cfg.wallDamping);
    cfg.maxSpeed       = j.value("max_speed", cfg.maxSpeed);

    if (j.contains("gravity"))    cfg.gravity   = readVec3(j["gravity"], cfg.gravity);
    if (j.contains("domain_min")) cfg.domainMin = readVec3(j["domain_min"], cfg.domainMin);
    if (j.contains("domain_max")) cfg.domainMax = readVec3(j["domain_max"], cfg.domainMax);

    return cfg;
}

} // namespace aquasph
