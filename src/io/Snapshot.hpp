#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace aquasph {

class Simulation;

// Particle snapshots: the fluid at a fixed simulated-time interval, for playback in the web
// console and for offline rendering in Blender. The file is self-describing (domain, faces,
// materials and obstacle geometry are in the header), so a reader needs nothing else. Byte
// layout: docs/snapshots.md.
namespace snapshot {
constexpr char kMagic[8] = {'A', 'Q', 'S', 'N', 'A', 'P', '1', '\n'};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kFrameMagic = 0x4D415246;  // "FRAM"
constexpr std::uint32_t kUnknownFrameCount = 0xFFFFFFFFu;
constexpr std::uint32_t kFlagMaterial = 1u << 0;   // a material byte per particle follows speed
constexpr std::uint32_t kFlagTruncated = 1u << 1;  // the size budget stopped export early
}  // namespace snapshot

struct SnapshotMaterial {
    std::string name;
    glm::vec3 absorption{0.0f};
    float restDensity = 1000.0f;
};

struct SnapshotObstacle {
    std::string name;
    std::uint8_t type = 0;  // ShapeType
    std::uint8_t axis = 1;
    glm::vec3 min{0.0f}, max{0.0f}, center{0.0f};
    float radius = 0.0f;
    float halfLength = 0.0f;
    // Heightfield only.
    float base = 0.0f;
    glm::vec2 origin{0.0f}, slope{0.0f};
    float minHeight = 0.0f, maxHeight = 0.0f;
    std::vector<glm::vec4> bumps;  // amplitude, centre x, centre z, sigma
};

struct SnapshotHeader {
    std::uint32_t version = 0;
    std::uint32_t frameCount = 0;
    std::uint32_t flags = 0;
    glm::vec3 domainMin{0.0f}, domainMax{0.0f};
    glm::vec3 quantMin{0.0f}, quantMax{0.0f};
    float particleRadius = 0.0f;
    float referenceSpeed = 1.0f;
    float spacing = 0.0f;
    float smoothingRadius = 0.0f;
    std::uint8_t faces[6] = {0, 0, 0, 0, 0, 0};  // FaceMode per face: -x +x -y +y -z +z
    std::vector<SnapshotMaterial> materials;
    std::vector<SnapshotObstacle> obstacles;
};

struct SnapshotFrame {
    float time = 0.0f;
    std::vector<glm::vec3> obstacleOffsets;
    std::vector<glm::vec3> positions;
    std::vector<float> speed;  // m/s, quantised against the header's reference speed
    std::vector<std::uint8_t> material;
};

class SnapshotWriter {
public:
    SnapshotWriter() = default;
    ~SnapshotWriter();
    SnapshotWriter(const SnapshotWriter&) = delete;
    SnapshotWriter& operator=(const SnapshotWriter&) = delete;

    // `interval` is in simulated seconds; `maxBytes` caps the file, after which export stops
    // and the header is flagged truncated rather than the disk filling up.
    bool open(const std::string& path, const Simulation& sim, float interval,
              std::uint64_t maxBytes, std::string& error);

    // Writes a frame once the simulation reaches the next export time (and at t = 0).
    bool sample(const Simulation& sim);
    bool writeFrame(const Simulation& sim);

    // Patches the frame count into the header. A file that is never closed (SIGKILL) keeps
    // kUnknownFrameCount and readers recover every complete frame.
    bool close();

    bool isOpen() const { return file_ != nullptr; }
    int frames() const { return frames_; }
    bool truncated() const { return truncated_; }
    std::uint64_t bytes() const { return bytes_; }
    float interval() const { return interval_; }
    float lastFrameTime() const { return lastTime_; }

private:
    std::FILE* file_ = nullptr;
    SnapshotHeader header_;
    float interval_ = 0.02f;
    float nextTime_ = 0.0f;
    float lastTime_ = -1.0f;
    std::uint64_t maxBytes_ = 0;
    std::uint64_t bytes_ = 0;
    int frames_ = 0;
    bool truncated_ = false;
    std::vector<std::uint8_t> buffer_;
};

// Reads a whole snapshot file. A partial final frame (the writer was killed mid-frame) is
// dropped, not reported as an error.
bool readSnapshotFile(const std::string& path, SnapshotHeader& header,
                      std::vector<SnapshotFrame>& frames, std::string& error);

}  // namespace aquasph
