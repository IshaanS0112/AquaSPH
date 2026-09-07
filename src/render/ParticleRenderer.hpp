#pragma once
#include <vector>
#include <cstddef>
#include <glm/glm.hpp>
#include "Shader.hpp"
#include "../core/Particle.hpp"

namespace aquasph {

// Points mode: particles drawn as speed-coloured circular sprites.
//
// This is NOT a legacy path kept for compatibility. It is the diagnostic
// view -- when a reconstructed surface looks wrong, the first question is
// whether the particle distribution underneath it is wrong, and only this
// mode answers that. It is also the honest performance baseline against
// which the screen-space surface's cost is measured. Both modes are
// available from the same binary at a keypress, which is what makes the
// comparison possible.
//
// COLOUR MAPPING. Speed is normalised against the SCENARIO's
// render.reference_speed, never against any physics limit. v1 divided by
// the integrator's velocity clamp while the integrator clamped to exactly
// that value, so during the interesting phase of a run nearly every
// particle mapped to 1.0 and the whole fluid rendered as a flat white
// sheet. Visualisation normalisation must never be tied to a physical
// bound, and physical bounds must never be chosen to suit a colour ramp.
class ParticleRenderer {
public:
    explicit ParticleRenderer(size_t maxParticles);
    ~ParticleRenderer();

    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    // Uploads position + normalised speed for the fluid particles only
    // (the array's leading boundary range is skipped). The buffer grows
    // when emitters push the count past its capacity.
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
