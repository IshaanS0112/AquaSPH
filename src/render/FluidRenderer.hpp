#pragma once
#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include "OffscreenTarget.hpp"
#include "Shader.hpp"
#include "../core/Particle.hpp"
#include "../scene/Scenario.hpp"

namespace aquasph {

// Screen-space fluid rendering, after van der Laan, Green & Sainz,
// "Screen Space Fluid Rendering with Curvature Flow" (I3D 2009).
//
// No mesh extraction and no marching cubes: the surface is never built as
// geometry at all. Particles are rasterised as sphere impostors into a
// depth buffer, that depth image is smoothed, and normals are taken from
// the smoothed depth. The cost is therefore per-pixel rather than
// per-particle-cubed, and the surface follows the particles exactly
// because it IS the particles, seen from where the camera is.
//
// The five passes, in order:
//
//  1. DEPTH. Each particle is one GL_POINT. The fragment shader
//     reconstructs the front of a sphere from gl_PointCoord, discards
//     fragments outside the disc, and writes the true sphere-surface
//     depth through gl_FragDepth -- so impostors intersect each other and
//     the scene correctly instead of behaving like flat billboards.
//     Linear eye-space depth goes to a float texture.
//
//  2. DEPTH SMOOTHING. A separable bilateral filter, run for a
//     quality-dependent number of iterations. Bilateral rather than
//     Gaussian because the filter MUST NOT smooth across a depth
//     discontinuity: blending the fluid's silhouette into the background
//     depth is exactly what produces the halo that gives screen-space
//     fluid away, and it is a bug, not a look. Background texels are
//     excluded from the sum entirely, and the remaining samples are
//     weighted by depth difference.
//
//  3. NORMALS. Finite differences of the view-space position
//     reconstructed from the SMOOTHED depth -- never from particle
//     geometry, which would put the particles back into the surface the
//     smoothing just removed. Forward and backward differences are
//     compared and the smaller taken, so a silhouette does not drag the
//     normal toward whatever is behind it.
//
//  4. THICKNESS. Particles rendered additively with the depth test off,
//     each contributing the chord length through its sphere, then blurred
//     lightly. This is the optical path length that drives absorption.
//
//  5. COMPOSITE. Schlick-Fresnel mixing a refracted and a reflected
//     sample; the refracted sample offset along the surface normal;
//     Beer-Lambert absorption over the thickness using the material's own
//     absorption coefficients; and a specular lobe per light from the
//     scenario's three-point rig.
class FluidRenderer {
public:
    struct Params {
        // World-space radius of the sphere each particle is drawn as.
        // Somewhat larger than half the particle spacing on purpose: the
        // impostors have to overlap or the depth image is a field of
        // separate dots that no amount of smoothing will join up.
        float particleRadius = 0.02f;
        int smoothIterations = 4;
        float blurRadiusPixels = 6.0f;
        // Larger = the bilateral filter guards discontinuities more
        // aggressively. In metres^-1: a depth step of 1/uDepthFalloff
        // roughly halves a sample's weight.
        float depthFalloff = 40.0f;
        float refractionStrength = 0.045f;
        // Tight enough to read as water rather than plastic, wide enough
        // that the highlight is actually visible on a surface this coarse:
        // at 120 the lobe was narrower than the reconstructed normals'
        // own resolution, so it fell between pixels and the fluid looked
        // like flat coloured glass.
        float specularPower = 48.0f;
        float specularIntensity = 1.15f;
        // Per-channel absorption, 1/m, from the material.
        glm::vec3 absorption{0.55f, 0.16f, 0.09f};
        // Scalar scattering coefficient, 1/m: how quickly the volume's own
        // colour saturates with optical path. Scalar on purpose -- see the
        // long note in the composite shader on why a per-channel version
        // renders water brown.
        float scatterCoefficient = 2.2f;
        // The colour the volume scatters back toward the eye. A deep,
        // desaturated teal: the one palette this project uses everywhere,
        // so a contact sheet compares physics rather than colour grading.
        glm::vec3 tint{0.105f, 0.430f, 0.500f};
        float thicknessScale = 1.0f;
    };

    FluidRenderer(int width, int height);

    void resize(int width, int height);

    // Uploads the fluid particle positions. Separate from render() so a
    // paused viewer can re-render the same state from a moving camera
    // without re-uploading it.
    void updateParticles(const std::vector<Particle>& particles, size_t firstFluid);

    // Renders the fluid over `scene` into `output`. `scene` supplies both
    // the background colour that refraction samples and the depth that
    // decides where scene geometry occludes the fluid.
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
