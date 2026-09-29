#pragma once
#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include "OffscreenTarget.hpp"
#include "Shader.hpp"
#include "../core/Particle.hpp"
#include "../scene/Scenario.hpp"

namespace aquasph {

// Screen-space fluid rendering, after van der Laan, Green & Sainz, "Screen Space Fluid
// Rendering with Curvature Flow" (I3D 2009).
class FluidRenderer {
public:
    struct Params {
        // World-space radius of the sphere each particle is drawn as.
        float particleRadius = 0.02f;
        int smoothIterations = 4;
        float blurRadiusPixels = 6.0f;
        // Larger = the bilateral filter guards discontinuities more aggressively.
        float depthFalloff = 40.0f;
        float refractionStrength = 0.045f;
        // Tight enough to read as water, wide enough for the highlight to survive coarse normals.
        float specularPower = 48.0f;
        float specularIntensity = 1.15f;
        // Per-channel absorption, 1/m, from the material.
        glm::vec3 absorption{0.55f, 0.16f, 0.09f};
        // Scalar scattering coefficient, 1/m: how quickly the volume's own colour saturates
        // with optical path.
        float scatterCoefficient = 2.2f;
        // The colour the volume scatters back toward the eye.
        glm::vec3 tint{0.105f, 0.430f, 0.500f};
        float thicknessScale = 1.0f;
    };

    FluidRenderer(int width, int height);

    void resize(int width, int height);

    // Uploads the fluid particle positions.
    void updateParticles(const std::vector<Particle>& particles, size_t firstFluid);

    // Renders the fluid over `scene` into `output`.
    void render(const OffscreenTarget& scene, OffscreenTarget& output,
                 const glm::mat4& view, const glm::mat4& proj,
                 float nearPlane, float farPlane, float fovYRadians,
                 const LightingSpec& lighting, const Params& params);

    size_t particleCount() const { return particleCount_; }

private:
    void ensureCapacity(size_t count);

    int width_;
    int height_;
    size_t capacity_ = 0;
    size_t particleCount_ = 0;
    gl::GLuint vao_ = 0;
    gl::GLuint vbo_ = 0;
    std::vector<float> cpu_;   // interleaved x,y,z per particle

    std::unique_ptr<OffscreenTarget> depthTarget_;
    std::unique_ptr<OffscreenTarget> smoothA_;
    std::unique_ptr<OffscreenTarget> smoothB_;
    std::unique_ptr<OffscreenTarget> thickness_;
    std::unique_ptr<OffscreenTarget> thicknessBlur_;

    std::unique_ptr<Shader> depthShader_;
    std::unique_ptr<Shader> blurShader_;
    std::unique_ptr<Shader> thicknessShader_;
    std::unique_ptr<Shader> thicknessBlurShader_;
    std::unique_ptr<Shader> compositeShader_;
    FullScreenTriangle quad_;
};

} // namespace aquasph
