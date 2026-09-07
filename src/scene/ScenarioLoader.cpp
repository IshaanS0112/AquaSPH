#include "ScenarioLoader.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace aquasph {

using json = nlohmann::json;

namespace {

glm::vec3 vec3Of(const json& j, const glm::vec3& def) {
    if (!j.is_array() || j.size() != 3) return def;
    return glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

glm::vec2 vec2Of(const json& j, const glm::vec2& def) {
    if (!j.is_array() || j.size() != 2) return def;
    return glm::vec2(j[0].get<float>(), j[1].get<float>());
}

glm::vec3 readVec3(const json& j, const char* key, const glm::vec3& def) {
    return j.contains(key) ? vec3Of(j[key], def) : def;
}

FaceMode faceModeOf(const std::string& s, const std::string& where) {
    if (s == "solid") return FaceMode::Solid;
    if (s == "open") return FaceMode::Open;
    if (s == "periodic") return FaceMode::Periodic;
    std::cerr << "[ScenarioLoader] Unknown face mode '" << s << "' in " << where
              << "; using 'solid'.\n";
    return FaceMode::Solid;
}

int axisOf(const json& j, const char* key, int def) {
    if (!j.contains(key)) return def;
    const json& v = j[key];
    if (v.is_number_integer()) return std::clamp(v.get<int>(), 0, 2);
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (s == "x") return 0;
        if (s == "y") return 1;
        if (s == "z") return 2;
    }
    return def;
}

Shape readShape(const json& j, const std::string& where) {
    Shape s;
    const std::string type = j.value("type", std::string("box"));
    if (type == "box" || type == "column") {
        s.type = ShapeType::Box;
    } else if (type == "sphere") {
        s.type = ShapeType::Sphere;
    } else if (type == "cylinder") {
        s.type = ShapeType::Cylinder;
    } else if (type == "heightfield" || type == "terrain") {
        s.type = ShapeType::Heightfield;
    } else {
        std::cerr << "[ScenarioLoader] Unknown shape type '" << type << "' in " << where
                  << "; using 'box'.\n";
        s.type = ShapeType::Box;
    }

    s.min = readVec3(j, "min", s.min);
    s.max = readVec3(j, "max", s.max);
    s.center = readVec3(j, "center", s.center);
    s.radius = j.value("radius", s.radius);
    s.halfLength = j.value("half_length", s.halfLength);
    s.axis = axisOf(j, "axis", s.axis);

    // A "column" is a box named for what it means in a dam break; it also
    // accepts the friendlier base/size form so a scenario author does not
    // have to add heights by hand.
    if (j.contains("base") && j.contains("size")) {
        const glm::vec3 base = vec3Of(j["base"], glm::vec3(0.0f));
        const glm::vec3 size = vec3Of(j["size"], glm::vec3(1.0f));
        s.min = base;
        s.max = base + size;
    }

    if (s.type == ShapeType::Heightfield && j.contains("field")) {
        const json& f = j["field"];
        s.field.base = f.value("base", s.field.base);
        s.field.origin = f.contains("origin") ? vec2Of(f["origin"], s.field.origin) : s.field.origin;
        s.field.slope = f.contains("slope") ? vec2Of(f["slope"], s.field.slope) : s.field.slope;
        s.field.minHeight = f.value("min_height", s.field.minHeight);
        s.field.maxHeight = f.value("max_height", s.field.maxHeight);
        if (f.contains("bumps") && f["bumps"].is_array()) {
            for (const json& b : f["bumps"]) {
                HeightfieldSpec::Bump bump;
                bump.amplitude = b.value("amplitude", 0.0f);
                bump.center = b.contains("center") ? vec2Of(b["center"], glm::vec2(0.0f)) : glm::vec2(0.0f);
                bump.sigma = b.value("sigma", 1.0f);
                s.field.bumps.push_back(bump);
            }
        }
    }
    return s;
}

TimeSeries readTimeSeries(const json& j, const std::string& where) {
    TimeSeries ts;
    if (j.is_number()) {   // a bare number is a constant
        ts.kind = TimeSeries::Kind::Constant;
        ts.value = j.get<float>();
        return ts;
    }
    const std::string kind = j.value("kind", std::string("constant"));
    if (kind == "constant")        ts.kind = TimeSeries::Kind::Constant;
    else if (kind == "ramp")       ts.kind = TimeSeries::Kind::Ramp;
    else if (kind == "pulse")      ts.kind = TimeSeries::Kind::Pulse;
    else if (kind == "sinusoidal") ts.kind = TimeSeries::Kind::Sinusoidal;
    else if (kind == "damped")     ts.kind = TimeSeries::Kind::Damped;
    else if (kind == "keyframes" || kind == "scripted") ts.kind = TimeSeries::Kind::Keyframes;
    else {
        std::cerr << "[ScenarioLoader] Unknown time-series kind '" << kind << "' in " << where
                  << "; using 'constant'.\n";
    }

    ts.value = j.value("value", ts.value);
    ts.offset = j.value("offset", ts.offset);
    ts.start = j.value("start", ts.start);
    ts.end = j.value("end", ts.end);
    ts.duration = j.value("duration", ts.duration);
    ts.period = j.value("period", ts.period);
    ts.phase = j.value("phase", ts.phase);
    ts.decay = j.value("decay", ts.decay);
    ts.rampTime = j.value("ramp_time", ts.rampTime);

    if (j.contains("keys") && j["keys"].is_array()) {
        for (const json& k : j["keys"]) {
            if (k.is_array() && k.size() == 2) {
                ts.keys.emplace_back(k[0].get<float>(), k[1].get<float>());
            }
        }
        std::sort(ts.keys.begin(), ts.keys.end(),
                   [](const glm::vec2& a, const glm::vec2& b) { return a.x < b.x; });
    }
    return ts;
}

Material readMaterial(const json& j) {
    Material m;
    m.name = j.value("name", m.name);
    m.restDensity = j.value("rest_density", m.restDensity);
    m.soundSpeed = j.value("sound_speed", m.soundSpeed);
    m.gamma = j.value("gamma", m.gamma);
    m.viscosity = j.value("viscosity", m.viscosity);
    m.surfaceTension = j.value("surface_tension", m.surfaceTension);
    m.absorption = readVec3(j, "absorption", m.absorption);
    return m;
}

} // namespace

bool ScenarioLoader::loadFile(const std::string& path, Scenario& out, std::string& error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "could not open '" + path + "'";
        return false;
    }
    json j;
    try {
        file >> j;
    } catch (const std::exception& e) {
        error = "failed to parse '" + path + "': " + e.what();
        return false;
    }

    // EVERYTHING BELOW IS INSIDE A try. The loader's documented contract
    // is that it reports and falls back rather than throwing, and until
    // this was added it did not honour that: nlohmann's accessors throw
    // type_error when a field holds the wrong type (a string where a
    // number is expected, an object where an array is), and nothing caught
    // it. A scenario file with one mistyped field would abort the process
    // with a bare `terminate called after throwing`, which is the least
    // useful possible response to a typo.
    try {
    Scenario s;
    s.name = j.value("name", std::filesystem::path(path).stem().string());
    s.description = j.value("description", s.description);
    s.approximation = j.value("approximation", s.approximation);
    const int tier = j.value("tier", 1);
    s.tier = (tier >= 2) ? Tier::Two : Tier::One;

    // --- domain ---
    if (j.contains("domain")) {
        const json& d = j["domain"];
        s.domain.min = readVec3(d, "min", s.domain.min);
        s.domain.max = readVec3(d, "max", s.domain.max);
        s.domain.boundaryParticles = d.value("boundary_particles", s.domain.boundaryParticles);
        s.domain.boundaryLayers = d.value("boundary_layers", s.domain.boundaryLayers);
        s.domain.boundarySpacingScale = d.value("boundary_spacing_scale", s.domain.boundarySpacingScale);
        s.domain.containmentDamping = d.value("containment_damping", s.domain.containmentDamping);
        if (d.contains("faces")) {
            const json& f = d["faces"];
            static const char* keys[6] = {"x_min", "x_max", "y_min", "y_max", "z_min", "z_max"};
            for (int i = 0; i < 6; ++i) {
                if (f.contains(keys[i])) {
                    s.domain.faces[i] = faceModeOf(f[keys[i]].get<std::string>(),
                                                    s.name + ".domain.faces." + keys[i]);
                }
            }
        }
    }

    s.gravity = readVec3(j, "gravity", s.gravity);

    // --- materials ---
    if (j.contains("materials") && j["materials"].is_array() && !j["materials"].empty()) {
        s.materials.clear();
        for (const json& m : j["materials"]) s.materials.push_back(readMaterial(m));
    }

    // --- fluid regions ---
    if (j.contains("fluid_regions")) {
        for (const json& r : j["fluid_regions"]) {
            FluidRegion region;
            region.shape = readShape(r, s.name + ".fluid_regions");
            region.material = r.value("material", region.material);
            region.velocity = readVec3(r, "velocity", region.velocity);
            const std::string prof = r.value("profile", std::string("uniform"));
            if (prof == "shear")       region.profile = FluidRegion::VelocityProfile::Shear;
            else if (prof == "vortex") region.profile = FluidRegion::VelocityProfile::Vortex;
            else                        region.profile = FluidRegion::VelocityProfile::Uniform;
            region.shearDir = readVec3(r, "shear_dir", region.shearDir);
            region.shearAxis = axisOf(r, "shear_axis", region.shearAxis);
            region.shearOrigin = r.value("shear_origin", region.shearOrigin);
            region.shearRate = r.value("shear_rate", region.shearRate);
            s.fluidRegions.push_back(region);
        }
    }

    // --- emitters ---
    if (j.contains("emitters")) {
        for (const json& e : j["emitters"]) {
            Emitter em;
            em.shape = readShape(e, s.name + ".emitters");
            em.material = e.value("material", em.material);
            em.direction = readVec3(e, "direction", em.direction);
            em.speed = e.value("speed", em.speed);
            em.startTime = e.value("start_time", em.startTime);
            em.endTime = e.value("end_time", em.endTime);
            if (e.contains("schedule")) {
                em.schedule = readTimeSeries(e["schedule"], s.name + ".emitters.schedule");
            }
            s.emitters.push_back(em);
        }
    }

    // --- sinks ---
    if (j.contains("sinks")) {
        for (const json& k : j["sinks"]) {
            Sink sk;
            sk.shape = readShape(k, s.name + ".sinks");
            sk.startTime = k.value("start_time", sk.startTime);
            sk.endTime = k.value("end_time", sk.endTime);
            s.sinks.push_back(sk);
        }
    }

    // --- obstacles ---
    if (j.contains("obstacles")) {
        for (const json& o : j["obstacles"]) {
            Obstacle obs;
            obs.name = o.value("name", obs.name);
            obs.shape = readShape(o, s.name + ".obstacles");
            if (o.contains("motion")) {
                const json& m = o["motion"];
                obs.motion.axis = readVec3(m, "axis", obs.motion.axis);
                if (m.contains("displacement")) {
                    obs.motion.displacement =
                        readTimeSeries(m["displacement"], s.name + ".obstacles.motion");
                }
            }
            s.obstacles.push_back(obs);
        }
    }

    // --- external forces ---
    if (j.contains("external_forces")) {
        for (const json& f : j["external_forces"]) {
            ExternalForce ef;
            ef.name = f.value("name", ef.name);
            ef.direction = readVec3(f, "direction", ef.direction);
            if (f.contains("magnitude")) {
                ef.magnitude = readTimeSeries(f["magnitude"], s.name + ".external_forces");
            }
            s.externalForces.push_back(ef);
        }
    }

    // --- wave generators ---
    if (j.contains("wave_generators")) {
        for (const json& w : j["wave_generators"]) {
            WaveGenerator wg;
            wg.name = w.value("name", wg.name);
            wg.axis = axisOf(w, "axis", wg.axis);
            wg.position = w.value("position", wg.position);
            wg.thickness = w.value("thickness", wg.thickness);
            wg.spanMin = readVec3(w, "span_min", s.domain.min);
            wg.spanMax = readVec3(w, "span_max", s.domain.max);
            const std::string mode = w.value("mode", std::string("sinusoidal"));
            if (mode == "pulse")              wg.mode = WaveGenerator::Mode::Pulse;
            else if (mode == "damped")        wg.mode = WaveGenerator::Mode::Damped;
            else if (mode == "superposition") wg.mode = WaveGenerator::Mode::Superposition;
            else                               wg.mode = WaveGenerator::Mode::Sinusoidal;
            wg.amplitude = w.value("amplitude", wg.amplitude);
            wg.period = w.value("period", wg.period);
            wg.phase = w.value("phase", wg.phase);
            wg.rampTime = w.value("ramp_time", wg.rampTime);
            wg.decay = w.value("decay", wg.decay);
            wg.startTime = w.value("start_time", wg.startTime);
            wg.duration = w.value("duration", wg.duration);
            wg.stillWaterDepth = w.value("still_water_depth", wg.stillWaterDepth);
            if (w.contains("components")) {
                for (const json& c : w["components"]) {
                    WaveComponent comp;
                    comp.amplitude = c.value("amplitude", comp.amplitude);
                    comp.period = c.value("period", comp.period);
                    comp.phase = c.value("phase", comp.phase);
                    wg.components.push_back(comp);
                }
            }
            s.waveGenerators.push_back(wg);
        }
    }

    // --- numerics ---
    if (j.contains("numerics")) {
        const json& n = j["numerics"];
        s.numerics.h = n.value("h", s.numerics.h);
        s.numerics.spacingRatio = n.value("spacing_ratio", s.numerics.spacingRatio);
        s.numerics.xsphEpsilon = n.value("xsph_epsilon", s.numerics.xsphEpsilon);
        s.numerics.boundaryFriction = n.value("boundary_friction", s.numerics.boundaryFriction);
        s.numerics.timestep.cflCoeff = n.value("cfl_coeff", s.numerics.timestep.cflCoeff);
        s.numerics.timestep.forceCoeff = n.value("force_coeff", s.numerics.timestep.forceCoeff);
        s.numerics.timestep.viscousCoeff = n.value("viscous_coeff", s.numerics.timestep.viscousCoeff);
        s.numerics.timestep.dtMin = n.value("dt_min", s.numerics.timestep.dtMin);
        s.numerics.timestep.dtMax = n.value("dt_max", s.numerics.timestep.dtMax);
    }

    // --- duration ---
    if (j.contains("duration")) {
        const json& d = j["duration"];
        s.duration.simulatedTime = d.value("simulated_time", s.duration.simulatedTime);
        s.duration.outputInterval = d.value("output_interval", s.duration.outputInterval);
        s.duration.maxSteps = d.value("max_steps", s.duration.maxSteps);
    }

    // --- camera / lighting / render ---
    if (j.contains("camera")) {
        const json& c = j["camera"];
        s.camera.target = readVec3(c, "target", s.camera.target);
        s.camera.distance = c.value("distance", s.camera.distance);
        s.camera.yawDeg = c.value("yaw_deg", s.camera.yawDeg);
        s.camera.pitchDeg = c.value("pitch_deg", s.camera.pitchDeg);
        s.camera.fovDeg = c.value("fov_deg", s.camera.fovDeg);
        s.camera.orbitRateDegPerSec = c.value("orbit_rate_deg_per_sec", s.camera.orbitRateDegPerSec);
    }
    if (j.contains("lighting")) {
        const json& l = j["lighting"];
        s.lighting.keyDirection = readVec3(l, "key_direction", s.lighting.keyDirection);
        s.lighting.keyColor = readVec3(l, "key_color", s.lighting.keyColor);
        s.lighting.keyIntensity = l.value("key_intensity", s.lighting.keyIntensity);
        s.lighting.fillDirection = readVec3(l, "fill_direction", s.lighting.fillDirection);
        s.lighting.fillColor = readVec3(l, "fill_color", s.lighting.fillColor);
        s.lighting.fillIntensity = l.value("fill_intensity", s.lighting.fillIntensity);
        s.lighting.rimDirection = readVec3(l, "rim_direction", s.lighting.rimDirection);
        s.lighting.rimColor = readVec3(l, "rim_color", s.lighting.rimColor);
        s.lighting.rimIntensity = l.value("rim_intensity", s.lighting.rimIntensity);
    }
    if (j.contains("render")) {
        const json& r = j["render"];
        const std::string mode = r.value("mode", std::string("surface"));
        s.render.mode = (mode == "points") ? RenderSettings::Mode::Points
                                            : RenderSettings::Mode::Surface;
        s.render.referenceSpeed = r.value("reference_speed", s.render.referenceSpeed);
        s.render.palette = r.value("palette", s.render.palette);
        s.render.pointSizePixels = r.value("point_size_pixels", s.render.pointSizePixels);
        s.render.showGrid = r.value("show_grid", s.render.showGrid);
        s.render.showDomainWireframe = r.value("show_domain_wireframe", s.render.showDomainWireframe);
        s.render.showFloor = r.value("show_floor", s.render.showFloor);
    }

    // --- metrics ---
    if (j.contains("metrics")) {
        const json& m = j["metrics"];
        s.metrics.trackSurgeFront = m.value("track_surge_front", s.metrics.trackSurgeFront);
        s.metrics.surgeAxis = axisOf(m, "surge_axis", s.metrics.surgeAxis);
        s.metrics.surgeOrigin = m.value("surge_origin", s.metrics.surgeOrigin);
        s.metrics.surgeColumnWidth = m.value("surge_column_width", s.metrics.surgeColumnWidth);
        s.metrics.surgeColumnHeight = m.value("surge_column_height", s.metrics.surgeColumnHeight);
        s.metrics.trackInundation = m.value("track_inundation", s.metrics.trackInundation);
        s.metrics.inundationDepth = m.value("inundation_depth", s.metrics.inundationDepth);
        if (m.contains("probes")) {
            for (const json& p : m["probes"]) {
                WaveProbe probe;
                probe.name = p.value("name", probe.name);
                probe.position = readVec3(p, "position", probe.position);
                probe.radius = p.value("radius", probe.radius);
                s.metrics.probes.push_back(probe);
            }
        }
    }

    out = std::move(s);
    return true;

    } catch (const std::exception& e) {
        error = "malformed value in '" + path + "': " + e.what() +
                 " (a field probably holds the wrong type -- see docs/scenarios.md"
                 " for the expected shape of each key)";
        return false;
    }
}

std::vector<std::string> ScenarioLoader::defaultSearchDirs() {
    return {"configs/scenarios", "../configs/scenarios", "../../configs/scenarios"};
}

std::string ScenarioLoader::resolve(const std::string& name,
                                     const std::vector<std::string>& searchDirs) {
    namespace fs = std::filesystem;
    // An explicit path wins over the search, so a scenario can live
    // anywhere during development.
    if (name.find('/') != std::string::npos || name.find(".json") != std::string::npos) {
        if (fs::exists(name)) return name;
    }
    for (const std::string& dir : searchDirs) {
        const fs::path candidate = fs::path(dir) / (name + ".json");
        std::error_code ec;
        if (fs::exists(candidate, ec)) return candidate.string();
    }
    return {};
}

std::vector<std::string> ScenarioLoader::listAvailable(const std::vector<std::string>& searchDirs) {
    namespace fs = std::filesystem;
    std::vector<std::string> names;
    std::set<std::string> seen;
    for (const std::string& dir : searchDirs) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        std::vector<std::string> local;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".json") continue;
            local.push_back(entry.path().stem().string());
        }
        std::sort(local.begin(), local.end());
        for (const std::string& n : local) {
            if (seen.insert(n).second) names.push_back(n);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace aquasph
