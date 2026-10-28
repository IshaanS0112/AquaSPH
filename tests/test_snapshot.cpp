// Snapshot files feed the console's 3D playback and the Blender renders, so what is read back has
// to be the simulated state, to within the documented quantisation.
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#ifdef _OPENMP
#include <omp.h>
#endif
#include "io/Snapshot.hpp"
#include "metrics/Metrics.hpp"
#include "scene/Scenario.hpp"
#include "scene/Simulation.hpp"

using namespace aquasph;

namespace {

Material fluid(const char* name, float rho0, glm::vec3 absorption) {
    Material m;
    m.name = name;
    m.restDensity = rho0;
    m.soundSpeed = 20.0f;
    m.viscosity = 2.0f;
    m.absorption = absorption;
    return m;
}

// Two materials, a gate on a keyframed track and a terrain bump: every header field and the
// per-frame obstacle offsets are exercised.
Scenario tank() {
    Scenario s;
    s.name = "snapshot_test";
    s.domain.min = glm::vec3(0.0f);
    s.domain.max = glm::vec3(0.6f, 0.4f, 0.2f);
    s.domain.faces[3] = FaceMode::Open;
    s.materials = MaterialTable{fluid("water", 1000.0f, {0.55f, 0.16f, 0.09f}),
                                fluid("oil", 900.0f, {0.10f, 0.25f, 0.90f})};

    FluidRegion water;
    water.shape.type = ShapeType::Box;
    water.shape.min = glm::vec3(0.02f, 0.0f, 0.0f);
    water.shape.max = glm::vec3(0.18f, 0.18f, 0.2f);
    water.material = "water";
    s.fluidRegions.push_back(water);
    FluidRegion oil = water;
    oil.shape.min = glm::vec3(0.40f, 0.0f, 0.0f);
    oil.shape.max = glm::vec3(0.50f, 0.10f, 0.2f);
    oil.material = "oil";
    s.fluidRegions.push_back(oil);

    Obstacle gate;
    gate.name = "gate";
    gate.shape.type = ShapeType::Box;
    gate.shape.min = glm::vec3(0.20f, 0.0f, 0.0f);
    gate.shape.max = glm::vec3(0.23f, 0.20f, 0.2f);
    gate.motion.axis = glm::vec3(0.0f, 1.0f, 0.0f);
    gate.motion.displacement.kind = TimeSeries::Kind::Keyframes;
    gate.motion.displacement.keys = {glm::vec2(0.0f, 0.0f), glm::vec2(0.1f, 0.05f)};
    s.obstacles.push_back(gate);

    Obstacle hill;
    hill.name = "hill";
    hill.shape.type = ShapeType::Heightfield;
    hill.shape.min = glm::vec3(0.3f, 0.0f, 0.0f);
    hill.shape.max = glm::vec3(0.6f, 0.1f, 0.2f);
    hill.shape.field.base = 0.0f;
    hill.shape.field.origin = glm::vec2(0.3f, 0.0f);
    hill.shape.field.maxHeight = 0.08f;
    hill.shape.field.minHeight = 0.0f;
    hill.shape.field.bumps.push_back({0.05f, glm::vec2(0.55f, 0.1f), 0.03f});
    s.obstacles.push_back(hill);

    s.numerics.h = 0.03f;
    s.numerics.spacingRatio = 0.5f;
    s.numerics.xsphEpsilon = 0.5f;
    s.duration.simulatedTime = 0.1f;
    s.duration.outputInterval = 0.02f;
    return s;
}

struct Captured {
    std::vector<float> times;
    std::vector<std::vector<glm::vec3>> positions;
    std::vector<std::vector<float>> speeds;
    std::vector<std::vector<std::uint8_t>> materials;
    std::vector<std::vector<glm::vec3>> offsets;
};

// Runs the scenario, writing snapshots, and keeps an exact copy of the state at each frame.
Captured writeRun(const Scenario& s, const std::string& path, int threads,
                  std::uint64_t maxBytes = 0) {
#ifdef _OPENMP
    omp_set_num_threads(threads);
#else
    (void)threads;
#endif
    Simulation sim(s);
    SnapshotWriter writer;
    std::string error;
    EXPECT_TRUE(writer.open(path, sim, 0.02f, maxBytes, error)) << error;

    Captured c;
    const auto sampleAndCapture = [&] {
        const int before = writer.frames();
        EXPECT_TRUE(writer.sample(sim));
        if (writer.frames() == before) return;
        c.times.push_back(sim.time());
        std::vector<glm::vec3> pos;
        std::vector<float> speed;
        std::vector<std::uint8_t> mat;
        for (size_t i = sim.boundaryCount(); i < sim.particles().size(); ++i) {
            const Particle& p = sim.particles()[i];
            pos.push_back(p.position);
            speed.push_back(glm::length(p.velocity));
            mat.push_back(p.material);
        }
        c.positions.push_back(pos);
        c.speeds.push_back(speed);
        c.materials.push_back(mat);
        std::vector<glm::vec3> off;
        for (const Obstacle& o : sim.scenario().obstacles) off.push_back(o.motion.translationAt(sim.time()));
        c.offsets.push_back(off);
    };
    sampleAndCapture();
    while (!sim.finished()) {
        sim.step();
        sampleAndCapture();
    }
    EXPECT_TRUE(writer.close());
    return c;
}

std::vector<char> bytesOf(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string tempPath(const char* name) {
    return testing::TempDir() + "aquasph_" + name + ".aqsnap";
}

}  // namespace

TEST(Snapshot, RoundTripsTheSimulatedStateWithinQuantisation) {
    const std::string path = tempPath("roundtrip");
    const Captured truth = writeRun(tank(), path, 2);
    ASSERT_GE(truth.times.size(), 5u);

    SnapshotHeader h;
    std::vector<SnapshotFrame> frames;
    std::string error;
    ASSERT_TRUE(readSnapshotFile(path, h, frames, error)) << error;

    EXPECT_EQ(h.frameCount, truth.times.size());
    EXPECT_EQ(h.flags & snapshot::kFlagMaterial, snapshot::kFlagMaterial);
    EXPECT_EQ(h.flags & snapshot::kFlagTruncated, 0u);
    EXPECT_EQ(h.faces[3], static_cast<std::uint8_t>(FaceMode::Open));
    ASSERT_EQ(h.materials.size(), 2u);
    EXPECT_EQ(h.materials[1].name, "oil");
    EXPECT_FLOAT_EQ(h.materials[1].absorption.z, 0.90f);
    ASSERT_EQ(h.obstacles.size(), 2u);
    EXPECT_EQ(h.obstacles[0].name, "gate");
    EXPECT_EQ(h.obstacles[1].type, static_cast<std::uint8_t>(ShapeType::Heightfield));
    ASSERT_EQ(h.obstacles[1].bumps.size(), 1u);
    EXPECT_FLOAT_EQ(h.obstacles[1].bumps[0].w, 0.03f);

    ASSERT_EQ(frames.size(), truth.times.size());
    const glm::vec3 step = (h.quantMax - h.quantMin) / 65535.0f;
    const float speedStep = h.referenceSpeed / 255.0f;
    for (size_t f = 0; f < frames.size(); ++f) {
        EXPECT_EQ(frames[f].time, truth.times[f]);
        ASSERT_EQ(frames[f].positions.size(), truth.positions[f].size());
        for (size_t i = 0; i < frames[f].positions.size(); ++i) {
            for (int a = 0; a < 3; ++a) {
                ASSERT_NEAR(frames[f].positions[i][a], truth.positions[f][i][a], 0.5f * step[a] + 1e-6f)
                    << "frame " << f << " particle " << i;
            }
            const float expected = std::min(truth.speeds[f][i], h.referenceSpeed);
            ASSERT_NEAR(frames[f].speed[i], expected, 0.5f * speedStep + 1e-5f);
            ASSERT_EQ(frames[f].material[i], truth.materials[f][i]);
        }
        ASSERT_EQ(frames[f].obstacleOffsets.size(), 2u);
        EXPECT_EQ(frames[f].obstacleOffsets[0], truth.offsets[f][0]);
    }
    // The gate is keyframed upward, so the recorded offsets must actually move.
    EXPECT_GT(frames.back().obstacleOffsets[0].y, frames.front().obstacleOffsets[0].y);
}

TEST(Snapshot, FileIsBitIdenticalAcrossThreadCounts) {
    const std::string one = tempPath("threads1");
    const std::string four = tempPath("threads4");
    writeRun(tank(), one, 1);
    writeRun(tank(), four, 4);
    const std::vector<char> a = bytesOf(one);
    const std::vector<char> b = bytesOf(four);
    ASSERT_FALSE(a.empty());
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(std::memcmp(a.data(), b.data(), a.size()), 0);
}

TEST(Snapshot, ReaderRecoversCompleteFramesFromAKilledWriter) {
    const std::string path = tempPath("killed");
    const Captured truth = writeRun(tank(), path, 2);
    std::vector<char> bytes = bytesOf(path);

    // What SIGKILL mid-frame leaves behind: no patched frame count and a torn last frame.
    const std::uint32_t unknown = snapshot::kUnknownFrameCount;
    std::memcpy(bytes.data() + 16, &unknown, sizeof(unknown));
    bytes.resize(bytes.size() - 7);
    const std::string torn = tempPath("torn");
    std::ofstream(torn, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));

    SnapshotHeader h;
    std::vector<SnapshotFrame> frames;
    std::string error;
    ASSERT_TRUE(readSnapshotFile(torn, h, frames, error)) << error;
    EXPECT_EQ(frames.size(), truth.times.size() - 1);
}

TEST(Snapshot, SizeBudgetStopsExportAndFlagsTheFile) {
    const std::string full = tempPath("full");
    writeRun(tank(), full, 2);
    const std::uint64_t budget = bytesOf(full).size() / 2;

    const std::string capped = tempPath("capped");
    const Captured written = writeRun(tank(), capped, 2, budget);
    EXPECT_LE(bytesOf(capped).size(), budget);

    SnapshotHeader h;
    std::vector<SnapshotFrame> frames;
    std::string error;
    ASSERT_TRUE(readSnapshotFile(capped, h, frames, error)) << error;
    EXPECT_EQ(h.flags & snapshot::kFlagTruncated, snapshot::kFlagTruncated);
    EXPECT_EQ(frames.size(), written.times.size());
    EXPECT_GE(frames.size(), 1u);
}

TEST(Snapshot, RejectsAFileThatIsNotASnapshot) {
    const std::string path = tempPath("garbage");
    std::ofstream(path, std::ios::binary) << std::string(256, 'x');
    SnapshotHeader h;
    std::vector<SnapshotFrame> frames;
    std::string error;
    EXPECT_FALSE(readSnapshotFile(path, h, frames, error));
    EXPECT_FALSE(error.empty());
}

TEST(MetricsTimeline, RecordsEveryOutputIntervalWithEnergies) {
    Scenario s = tank();
    s.obstacles.clear();
    Simulation sim(s);
    MetricsCollector metrics(sim.scenario());
    while (!sim.finished()) {
        metrics.observeStep(sim.step());
        metrics.sample(sim);
    }
    metrics.finish(sim, 1.0, 1.0, "test", 1, "test");

    const TimelineSeries& tl = metrics.report().timeline;
    ASSERT_GE(tl.time.size(), 4u);
    for (size_t n : {tl.dt.size(), tl.maxSpeed.size(), tl.densityAvg.size(), tl.nearRest.size(),
                     tl.fluid.size(), tl.volume.size(), tl.kineticEnergy.size(), tl.potentialEnergy.size()}) {
        EXPECT_EQ(n, tl.time.size());
    }
    // Columns released from rest: potential energy is spent, kinetic energy appears.
    EXPECT_LT(tl.potentialEnergy.back(), tl.potentialEnergy.front());
    EXPECT_GT(tl.kineticEnergy.back(), tl.kineticEnergy.front());
    for (double ke : tl.kineticEnergy) EXPECT_GE(ke, 0.0);

    const nlohmann::json j = nlohmann::json::parse(metrics.report().toJson());
    ASSERT_TRUE(j.contains("timeline"));
    EXPECT_EQ(j["timeline"]["kinetic_energy"].size(), tl.time.size());
    EXPECT_FALSE(j.contains("snapshots"));
}
