#include "Snapshot.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

#include "../scene/Simulation.hpp"

namespace aquasph {
namespace {

static_assert(sizeof(float) == 4, "the snapshot format stores 32-bit floats");

// Fixed offsets of the two fields close() patches.
constexpr long kFrameCountOffset = 16;
constexpr long kFlagsOffset = 20;

bool hostIsLittleEndian() {
    const std::uint16_t probe = 1;
    std::uint8_t first = 0;
    std::memcpy(&first, &probe, 1);
    return first == 1;
}

template <typename T>
void put(std::vector<std::uint8_t>& out, const T& v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

void putVec3(std::vector<std::uint8_t>& out, const glm::vec3& v) {
    put(out, v.x);
    put(out, v.y);
    put(out, v.z);
}

void putString(std::vector<std::uint8_t>& out, const std::string& s) {
    const auto n = static_cast<std::uint8_t>(std::min<size_t>(s.size(), 255));
    put(out, n);
    out.insert(out.end(), s.begin(), s.begin() + n);
}

void padTo4(std::vector<std::uint8_t>& out) {
    while (out.size() % 4 != 0) out.push_back(0);
}

class Cursor {
public:
    Cursor(const std::vector<std::uint8_t>& data, size_t pos) : data_(data), pos_(pos) {}

    template <typename T>
    bool get(T& v) {
        if (pos_ + sizeof(T) > data_.size()) return false;
        std::memcpy(&v, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }
    bool getVec3(glm::vec3& v) { return get(v.x) && get(v.y) && get(v.z); }
    bool getString(std::string& s) {
        std::uint8_t n = 0;
        if (!get(n) || pos_ + n > data_.size()) return false;
        s.assign(reinterpret_cast<const char*>(data_.data() + pos_), n);
        pos_ += n;
        return true;
    }
    size_t pos() const { return pos_; }

private:
    const std::vector<std::uint8_t>& data_;
    size_t pos_;
};

std::uint16_t quantise(float v, float lo, float hi) {
    if (!std::isfinite(v)) return 0;
    const float u = std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
    return static_cast<std::uint16_t>(std::lround(u * 65535.0f));
}

float dequantise(std::uint16_t q, float lo, float hi) {
    return lo + (hi - lo) * (static_cast<float>(q) / 65535.0f);
}

std::vector<std::uint8_t> encodeHeader(const SnapshotHeader& h) {
    std::vector<std::uint8_t> out;
    out.insert(out.end(), snapshot::kMagic, snapshot::kMagic + 8);
    put(out, snapshot::kVersion);
    put(out, std::uint32_t{0});  // header size, patched below
    put(out, h.frameCount);
    put(out, h.flags);
    putVec3(out, h.domainMin);
    putVec3(out, h.domainMax);
    putVec3(out, h.quantMin);
    putVec3(out, h.quantMax);
    put(out, h.particleRadius);
    put(out, h.referenceSpeed);
    put(out, h.spacing);
    put(out, h.smoothingRadius);
    out.insert(out.end(), h.faces, h.faces + 6);
    put(out, static_cast<std::uint16_t>(h.materials.size()));
    put(out, static_cast<std::uint32_t>(h.obstacles.size()));
    for (const SnapshotMaterial& m : h.materials) {
        putString(out, m.name);
        putVec3(out, m.absorption);
        put(out, m.restDensity);
    }
    for (const SnapshotObstacle& o : h.obstacles) {
        put(out, o.type);
        put(out, o.axis);
        putString(out, o.name);
        putVec3(out, o.min);
        putVec3(out, o.max);
        putVec3(out, o.center);
        put(out, o.radius);
        put(out, o.halfLength);
        if (o.type == static_cast<std::uint8_t>(ShapeType::Heightfield)) {
            put(out, o.base);
            put(out, o.origin.x);
            put(out, o.origin.y);
            put(out, o.slope.x);
            put(out, o.slope.y);
            put(out, o.minHeight);
            put(out, o.maxHeight);
            put(out, static_cast<std::uint32_t>(o.bumps.size()));
            for (const glm::vec4& b : o.bumps) {
                put(out, b.x);
                put(out, b.y);
                put(out, b.z);
                put(out, b.w);
            }
        }
    }
    padTo4(out);
    const auto size = static_cast<std::uint32_t>(out.size());
    std::memcpy(out.data() + 12, &size, sizeof(size));
    return out;
}

size_t frameBytes(size_t count, size_t obstacles, bool withMaterial) {
    size_t n = 16 + 12 * obstacles + 6 * count + count + (withMaterial ? count : 0);
    return (n + 3) & ~size_t{3};
}

}  // namespace

SnapshotWriter::~SnapshotWriter() { close(); }

bool SnapshotWriter::open(const std::string& path, const Simulation& sim, float interval,
                          std::uint64_t maxBytes, std::string& error) {
    if (!hostIsLittleEndian()) {
        error = "snapshots are little-endian and this host is not";
        return false;
    }
    close();
    const Scenario& sc = sim.scenario();

    SnapshotHeader h;
    h.version = snapshot::kVersion;
    h.frameCount = snapshot::kUnknownFrameCount;
    h.domainMin = sc.domain.min;
    h.domainMax = sc.domain.max;
    // Particles can sit a fraction of h outside a face before containment or removal acts.
    const glm::vec3 extent = sc.domain.max - sc.domain.min;
    const float margin = std::max(4.0f * sim.smoothingRadius(),
                                  0.02f * std::max({extent.x, extent.y, extent.z}));
    h.quantMin = sc.domain.min - glm::vec3(margin);
    h.quantMax = sc.domain.max + glm::vec3(margin);
    h.particleRadius = 0.5f * sim.spacing();
    h.referenceSpeed = std::max(sc.render.referenceSpeed, 1.0e-3f);
    h.spacing = sim.spacing();
    h.smoothingRadius = sim.smoothingRadius();
    for (int f = 0; f < 6; ++f) h.faces[f] = static_cast<std::uint8_t>(sc.domain.faces[f]);
    for (const Material& m : sc.materials) {
        h.materials.push_back({m.name, m.absorption, m.restDensity});
    }
    if (h.materials.size() > 1) h.flags |= snapshot::kFlagMaterial;
    for (const Obstacle& ob : sc.obstacles) {
        SnapshotObstacle o;
        o.name = ob.name;
        o.type = static_cast<std::uint8_t>(ob.shape.type);
        o.axis = static_cast<std::uint8_t>(std::clamp(ob.shape.axis, 0, 2));
        o.min = ob.shape.min;
        o.max = ob.shape.max;
        o.center = ob.shape.center;
        o.radius = ob.shape.radius;
        o.halfLength = ob.shape.halfLength;
        if (ob.shape.type == ShapeType::Heightfield) {
            const HeightfieldSpec& f = ob.shape.field;
            o.base = f.base;
            o.origin = f.origin;
            o.slope = f.slope;
            o.minHeight = f.minHeight;
            o.maxHeight = f.maxHeight;
            for (const HeightfieldSpec::Bump& b : f.bumps) {
                o.bumps.emplace_back(b.amplitude, b.center.x, b.center.y, b.sigma);
            }
        }
        h.obstacles.push_back(std::move(o));
    }

    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) {
        error = "cannot open snapshot file '" + path + "' for writing";
        return false;
    }
    const std::vector<std::uint8_t> bytes = encodeHeader(h);
    if (std::fwrite(bytes.data(), 1, bytes.size(), file_) != bytes.size()) {
        error = "cannot write snapshot header to '" + path + "'";
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    header_ = std::move(h);
    interval_ = std::max(interval, 1.0e-6f);
    nextTime_ = 0.0f;
    lastTime_ = -1.0f;
    maxBytes_ = maxBytes;
    bytes_ = bytes.size();
    frames_ = 0;
    truncated_ = false;
    return true;
}

bool SnapshotWriter::sample(const Simulation& sim) {
    if (!file_ || truncated_) return true;
    if (frames_ > 0 && sim.time() < nextTime_) return true;
    // A fixed grid of export times, so frames do not drift by one step's dt each.
    while (nextTime_ <= sim.time()) nextTime_ += interval_;
    return writeFrame(sim);
}

bool SnapshotWriter::writeFrame(const Simulation& sim) {
    if (!file_ || truncated_) return true;
    const auto& particles = sim.particles();
    const size_t begin = sim.boundaryCount();
    const size_t count = particles.size() > begin ? particles.size() - begin : 0;
    const bool withMaterial = (header_.flags & snapshot::kFlagMaterial) != 0;
    const size_t size = frameBytes(count, header_.obstacles.size(), withMaterial);
    if (maxBytes_ > 0 && bytes_ + size > maxBytes_) {
        truncated_ = true;
        return true;
    }

    buffer_.clear();
    buffer_.reserve(size);
    put(buffer_, snapshot::kFrameMagic);
    put(buffer_, sim.time());
    put(buffer_, static_cast<std::uint32_t>(count));
    put(buffer_, std::uint32_t{0});
    for (const Obstacle& ob : sim.scenario().obstacles) putVec3(buffer_, ob.motion.translationAt(sim.time()));
    const glm::vec3 lo = header_.quantMin;
    const glm::vec3 hi = header_.quantMax;
    for (size_t i = begin; i < particles.size(); ++i) {
        const glm::vec3& p = particles[i].position;
        put(buffer_, quantise(p.x, lo.x, hi.x));
        put(buffer_, quantise(p.y, lo.y, hi.y));
        put(buffer_, quantise(p.z, lo.z, hi.z));
    }
    const float vref = header_.referenceSpeed;
    for (size_t i = begin; i < particles.size(); ++i) {
        const float s = glm::length(particles[i].velocity);
        const float u = std::isfinite(s) ? std::clamp(s / vref, 0.0f, 1.0f) : 1.0f;
        buffer_.push_back(static_cast<std::uint8_t>(std::lround(u * 255.0f)));
    }
    if (withMaterial) {
        for (size_t i = begin; i < particles.size(); ++i) buffer_.push_back(particles[i].material);
    }
    padTo4(buffer_);

    if (std::fwrite(buffer_.data(), 1, buffer_.size(), file_) != buffer_.size()) return false;
    bytes_ += buffer_.size();
    ++frames_;
    lastTime_ = sim.time();
    return true;
}

bool SnapshotWriter::close() {
    if (!file_) return true;
    bool ok = true;
    const auto count = static_cast<std::uint32_t>(frames_);
    std::uint32_t flags = header_.flags | (truncated_ ? snapshot::kFlagTruncated : 0u);
    ok = ok && std::fseek(file_, kFrameCountOffset, SEEK_SET) == 0;
    ok = ok && std::fwrite(&count, sizeof(count), 1, file_) == 1;
    ok = ok && std::fseek(file_, kFlagsOffset, SEEK_SET) == 0;
    ok = ok && std::fwrite(&flags, sizeof(flags), 1, file_) == 1;
    ok = (std::fclose(file_) == 0) && ok;
    file_ = nullptr;
    return ok;
}

bool readSnapshotFile(const std::string& path, SnapshotHeader& header,
                      std::vector<SnapshotFrame>& frames, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open '" + path + "'";
        return false;
    }
    const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
    if (data.size() < 100 || std::memcmp(data.data(), snapshot::kMagic, 8) != 0) {
        error = "not an AquaSPH snapshot file";
        return false;
    }

    header = SnapshotHeader{};
    Cursor c(data, 8);
    std::uint32_t headerBytes = 0;
    std::uint16_t materialCount = 0;
    std::uint32_t obstacleCount = 0;
    bool ok = c.get(header.version) && c.get(headerBytes) && c.get(header.frameCount) &&
              c.get(header.flags) && c.getVec3(header.domainMin) && c.getVec3(header.domainMax) &&
              c.getVec3(header.quantMin) && c.getVec3(header.quantMax) &&
              c.get(header.particleRadius) && c.get(header.referenceSpeed) &&
              c.get(header.spacing) && c.get(header.smoothingRadius);
    for (int f = 0; ok && f < 6; ++f) ok = c.get(header.faces[f]);
    ok = ok && c.get(materialCount) && c.get(obstacleCount);
    if (!ok || header.version != snapshot::kVersion || headerBytes > data.size()) {
        error = "unsupported or corrupt snapshot header";
        return false;
    }
    for (std::uint16_t i = 0; ok && i < materialCount; ++i) {
        SnapshotMaterial m;
        ok = c.getString(m.name) && c.getVec3(m.absorption) && c.get(m.restDensity);
        header.materials.push_back(m);
    }
    for (std::uint32_t i = 0; ok && i < obstacleCount; ++i) {
        SnapshotObstacle o;
        ok = c.get(o.type) && c.get(o.axis) && c.getString(o.name) && c.getVec3(o.min) &&
             c.getVec3(o.max) && c.getVec3(o.center) && c.get(o.radius) && c.get(o.halfLength);
        if (ok && o.type == static_cast<std::uint8_t>(ShapeType::Heightfield)) {
            std::uint32_t bumps = 0;
            ok = c.get(o.base) && c.get(o.origin.x) && c.get(o.origin.y) && c.get(o.slope.x) &&
                 c.get(o.slope.y) && c.get(o.minHeight) && c.get(o.maxHeight) && c.get(bumps);
            for (std::uint32_t b = 0; ok && b < bumps; ++b) {
                glm::vec4 v;
                ok = c.get(v.x) && c.get(v.y) && c.get(v.z) && c.get(v.w);
                o.bumps.push_back(v);
            }
        }
        header.obstacles.push_back(std::move(o));
    }
    if (!ok || c.pos() > headerBytes) {
        error = "corrupt snapshot header";
        return false;
    }

    frames.clear();
    const bool withMaterial = (header.flags & snapshot::kFlagMaterial) != 0;
    const glm::vec3 lo = header.quantMin;
    const glm::vec3 hi = header.quantMax;
    size_t pos = headerBytes;
    while (pos + 16 <= data.size()) {
        if (header.frameCount != snapshot::kUnknownFrameCount && frames.size() >= header.frameCount) break;
        Cursor f(data, pos);
        std::uint32_t magic = 0, count = 0, reserved = 0;
        SnapshotFrame frame;
        f.get(magic);
        f.get(frame.time);
        f.get(count);
        f.get(reserved);
        if (magic != snapshot::kFrameMagic) {
            error = "corrupt snapshot frame at byte " + std::to_string(pos);
            return false;
        }
        const size_t size = frameBytes(count, header.obstacles.size(), withMaterial);
        if (pos + size > data.size()) break;  // partial final frame: the writer was killed
        frame.obstacleOffsets.resize(header.obstacles.size());
        for (glm::vec3& o : frame.obstacleOffsets) f.getVec3(o);
        frame.positions.resize(count);
        for (glm::vec3& p : frame.positions) {
            std::uint16_t qx = 0, qy = 0, qz = 0;
            f.get(qx);
            f.get(qy);
            f.get(qz);
            p = glm::vec3(dequantise(qx, lo.x, hi.x), dequantise(qy, lo.y, hi.y),
                          dequantise(qz, lo.z, hi.z));
        }
        frame.speed.resize(count);
        for (float& s : frame.speed) {
            std::uint8_t q = 0;
            f.get(q);
            s = header.referenceSpeed * (static_cast<float>(q) / 255.0f);
        }
        if (withMaterial) {
            frame.material.resize(count);
            for (std::uint8_t& m : frame.material) f.get(m);
        }
        frames.push_back(std::move(frame));
        pos += size;
    }
    return true;
}

}  // namespace aquasph
