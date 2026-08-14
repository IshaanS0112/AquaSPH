#pragma once
#include <vector>
#include <cstddef>
#include <glm/glm.hpp>
#include "Shader.hpp"
#include "../core/Particle.hpp"

namespace aquasph {

// Renders the current particle state as GL_POINTS, sized uniformly and
// colored by speed (slow -> deep blue, fast -> white) -- a cheap,
// physically meaningful visual: the initial floor impact and any
// still-unsettled turbulence stand out immediately from the settling
// pool. Deliberately not instanced spheres or billboarded quads -- the
// speed colormap is the diagnostic payload, and point sprites deliver it
// at a fraction of the complexity and fill cost of instanced geometry.
class ParticleRenderer {
public:
    explicit ParticleRenderer(size_t maxParticles);
    ~ParticleRenderer();

    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    // Re-uploads position + normalized speed for every particle.
    // `speedForFullColor` is the speed (m/s) that maps to the "fast"
    // end of the colormap -- pass cfg.maxSpeed so the color scale means
    // roughly the same thing across different config files.
    void updateParticles(const std::vector<Particle>& particles, float speedForFullColor);

    void draw(const glm::mat4& view, const glm::mat4& proj, float pointSizePixels) const;

private:
    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;
    size_t maxParticles_;
    size_t particleCount_ = 0;
    std::vector<float> cpuBuffer_; // interleaved x,y,z,speedNorm per particle
    Shader shader_;
};

} // namespace aquasph
