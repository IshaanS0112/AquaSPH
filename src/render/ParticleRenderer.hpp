#pragma once
#include <vector>
#include <cstddef>
#include <glm/glm.hpp>
#include "Shader.hpp"
#include "../core/Particle.hpp"

namespace aquasph {

// Points mode: particles drawn as speed-coloured circular sprites.
class ParticleRenderer {
public:
    explicit ParticleRenderer(size_t maxParticles);
    ~ParticleRenderer();

    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    // Uploads position + normalised speed for the fluid particles only (the array's leading
    // boundary range is skipped).
    void updateParticles(const std::vector<Particle>& particles, size_t firstFluid,
                          float referenceSpeed);

    void draw(const glm::mat4& view, const glm::mat4& proj, float pointSizePixels) const;

private:
    void ensureCapacity(size_t count);

    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;
    size_t capacity_ = 0;
    size_t particleCount_ = 0;
    std::vector<float> cpuBuffer_; // interleaved x,y,z,speedNorm per particle
    Shader shader_;
};

} // namespace aquasph
