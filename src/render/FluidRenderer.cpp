#include "FluidRenderer.hpp"
#include "GLLoader.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>

namespace aquasph {

using namespace gl;

namespace {

// Sentinel written where no particle covered a pixel. Any value beyond
// the far plane works; it must be large enough that the "is this
// background?" test in every later pass is unambiguous.
constexpr float kBackgroundDepth = 1.0e6f;

// Shared by every full-screen pass. The three clip-space corners are
// generated from gl_VertexID, so no vertex buffer exists to get wrong.
const char* kFullScreenVS = R"GLSL(
#version 330 core
out vec2 vUV;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// --- Pass 1: sphere-impostor depth ------------------------------------
const char* kDepthVS = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPos;

uniform mat4 uView;
uniform mat4 uProj;
uniform float uPointScale;   // viewportHeight / (2 tan(fovY/2))
uniform float uRadius;
uniform float uMaxPointSize;

out vec3 vViewPos;

void main() {
    vec4 viewPos = uView * vec4(aPos, 1.0);
    vViewPos = viewPos.xyz;
    gl_Position = uProj * viewPos;
    // Perspective-correct sprite size: the projected diameter of a sphere
    // of radius uRadius at this distance. Clamped because gl_PointSize has
    // an implementation-defined ceiling and exceeding it silently shrinks
    // the sprite, which would punch holes in the surface up close.
    float size = uPointScale * uRadius / max(-viewPos.z, 1e-4);
    gl_PointSize = clamp(size, 1.0, uMaxPointSize);
}
)GLSL";

const char* kDepthFS = R"GLSL(
#version 330 core
in vec3 vViewPos;

uniform mat4 uProj;
uniform float uRadius;

out float outDepth;

void main() {
    // Rebuild the sphere from the sprite's local coordinates. gl_PointCoord
    // has its origin at the TOP left, so y is flipped to match the
    // bottom-up view space the rest of the pipeline works in.
    vec2 c = gl_PointCoord * 2.0 - 1.0;
    c.y = -c.y;
    float r2 = dot(c, c);
    if (r2 > 1.0) discard;           // outside the disc: not part of the sphere

    vec3 n = vec3(c, sqrt(1.0 - r2));
    vec3 spherePos = vViewPos + n * uRadius;

    // TRUE sphere-surface depth, not the sprite's flat centre depth. This
    // is what makes impostors intersect each other and the scene
    // correctly instead of behaving like cardboard cut-outs.
    vec4 clip = uProj * vec4(spherePos, 1.0);
    gl_FragDepth = (clip.z / clip.w) * 0.5 + 0.5;

    outDepth = -spherePos.z;         // positive distance along the view axis
}
)GLSL";

// --- Pass 2: separable bilateral depth smoothing -----------------------
const char* kBlurFS = R"GLSL(
#version 330 core
in vec2 vUV;

uniform sampler2D uDepth;
uniform vec2 uTexel;
uniform vec2 uDir;              // (1,0) horizontally, (0,1) vertically
uniform float uRadiusPixels;
uniform float uDepthFalloff;
uniform float uBackground;

out float outDepth;

void main() {
    float centre = texture(uDepth, vUV).r;
    if (centre >= uBackground) { outDepth = centre; return; }

    float sigma = max(uRadiusPixels * 0.5, 1e-3);
    float twoSigma2 = 2.0 * sigma * sigma;
    int R = int(uRadiusPixels);

    float sum = 0.0;
    float wsum = 0.0;
    for (int i = -R; i <= R; ++i) {
        float s = texture(uDepth, vUV + uDir * uTexel * float(i)).r;

        // THE HALO FIX, PART ONE: background texels are not smoothed
        // against at all. Averaging the silhouette with a depth of 1e6
        // would drag the edge of the fluid a long way back and produce
        // the bright fringe that gives screen-space fluid away.
        if (s >= uBackground) continue;

        float ws = exp(-float(i * i) / twoSigma2);

        // PART TWO: samples are also weighted down by how far their depth
        // is from the centre's, so two separate sheets of fluid that
        // happen to overlap on screen do not bleed into one another.
        float dd = (s - centre) * uDepthFalloff;
        float wd = exp(-dd * dd);

        float w = ws * wd;
        sum += s * w;
        wsum += w;
    }
    outDepth = wsum > 0.0 ? sum / wsum : centre;
}
)GLSL";

// --- Pass 4: additive thickness ---------------------------------------
const char* kThicknessVS = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uView;
uniform mat4 uProj;
uniform float uPointScale;
uniform float uRadius;
uniform float uMaxPointSize;
void main() {
    vec4 viewPos = uView * vec4(aPos, 1.0);
    gl_Position = uProj * viewPos;
    float size = uPointScale * uRadius / max(-viewPos.z, 1e-4);
    gl_PointSize = clamp(size, 1.0, uMaxPointSize);
}
)GLSL";

const char* kThicknessFS = R"GLSL(
#version 330 core
uniform float uRadius;
out float outThickness;
void main() {
    vec2 c = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(c, c);
    if (r2 > 1.0) discard;
    // Chord length through the sphere at this offset from its centre --
    // the actual optical path a ray takes, not a constant per particle.
    // Summed additively with the depth test off, this is the total
    // thickness of fluid along the view ray, which is what Beer-Lambert
    // absorption needs.
    outThickness = 2.0 * uRadius * sqrt(1.0 - r2);
}
)GLSL";

const char* kThicknessBlurFS = R"GLSL(
#version 330 core
in vec2 vUV;
uniform sampler2D uThickness;
uniform vec2 uTexel;
out float outThickness;
void main() {
    // A plain separable-ish 3x3 Gaussian. Thickness has no silhouettes to
    // preserve -- it is already a smooth accumulation -- so the bilateral
    // machinery would only cost time here.
    float sum = 0.0;
    float wsum = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            float w = exp(-float(x * x + y * y) / 4.0);
            sum += texture(uThickness, vUV + vec2(x, y) * uTexel).r * w;
            wsum += w;
        }
    }
    outThickness = sum / wsum;
}
)GLSL";

// --- Pass 5: composite ------------------------------------------------
const char* kCompositeFS = R"GLSL(
#version 330 core
in vec2 vUV;

uniform sampler2D uDepth;        // smoothed linear eye depth
uniform sampler2D uThickness;
uniform sampler2D uSceneColor;
uniform sampler2D uSceneDepth;   // hardware depth of the environment

uniform mat4 uInvProj;
uniform mat4 uInvView;
uniform vec2 uTexel;
uniform float uBackground;
uniform float uNear;
uniform float uFar;

uniform vec3 uAbsorption;
uniform vec3 uTint;
uniform float uRefract;
uniform float uScatter;
uniform float uThicknessScale;
uniform float uSpecPower;
uniform float uSpecIntensity;

uniform vec3 uKeyDir;   uniform vec3 uKeyColor;   uniform float uKeyIntensity;
uniform vec3 uFillDir;  uniform vec3 uFillColor;  uniform float uFillIntensity;
uniform vec3 uRimDir;   uniform vec3 uRimColor;   uniform float uRimIntensity;

out vec4 FragColor;

// Eye-space position of the pixel at `uv` given its linear eye depth.
vec3 viewPosOf(vec2 uv, float eyeDepth) {
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 clip = vec4(ndc, -1.0, 1.0);
    vec4 v = uInvProj * clip;
    vec3 ray = v.xyz / v.w;
    ray /= max(-ray.z, 1e-6);      // normalise so the ray has z = -1
    return ray * eyeDepth;
}

float linearizeSceneDepth(float d) {
    float z = d * 2.0 - 1.0;
    return (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));
}

// View-space position of a neighbouring pixel, falling back to the
// centre pixel's depth where the neighbour is background.
vec3 neighbourPos(vec2 uv, float centreDepth) {
    float d = texture(uDepth, uv).r;
    return viewPosOf(uv, d >= uBackground ? centreDepth : d);
}

// Pick whichever of the forward/backward difference is smaller. At a
// silhouette one of the two straddles the edge and is huge; taking the
// smaller keeps the normal on the surface instead of tipping it toward
// whatever lies behind.
vec3 minDiff(vec3 centre, vec3 plus, vec3 minus) {
    vec3 a = plus - centre;
    vec3 b = centre - minus;
    return (dot(a, a) < dot(b, b)) ? a : b;
}

// A cheap analytic environment: a cool sky above, a darker ground below,
// with the horizon softened. Deliberately not a loaded HDR cubemap --
// this exists to give the specular and the reflection something plausible
// to pick up, and a single texture asset would be one more thing to ship
// and to get wrong.
vec3 environmentColor(vec3 dir) {
    float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 sky = vec3(0.40, 0.52, 0.62);
    vec3 ground = vec3(0.075, 0.085, 0.100);
    return mix(ground, sky, smoothstep(0.30, 0.80, t));
}

void main() {
    vec3 scene = texture(uSceneColor, vUV).rgb;
    float depth = texture(uDepth, vUV).r;

    if (depth >= uBackground) { FragColor = vec4(scene, 1.0); return; }

    // Environment geometry in front of the fluid wins.
    float sceneEye = linearizeSceneDepth(texture(uSceneDepth, vUV).r);
    if (sceneEye < depth) { FragColor = vec4(scene, 1.0); return; }

    vec3 P = viewPosOf(vUV, depth);
    // A neighbour that is BACKGROUND is clamped to the centre's depth
    // rather than used at 1e6. Sampling the sentinel produces a position
    // a kilometre away, a wildly wrong finite difference, and a garbage
    // normal -- which showed up as a ring of black speckle all around the
    // fluid's silhouette and along every thin sheet. Clamping makes the
    // difference purely lateral there, so a silhouette pixel gets a
    // camera-facing normal instead of a random one.
    vec3 Px  = neighbourPos(vUV + vec2(uTexel.x, 0.0), depth);
    vec3 Pxm = neighbourPos(vUV - vec2(uTexel.x, 0.0), depth);
    vec3 Py  = neighbourPos(vUV + vec2(0.0, uTexel.y), depth);
    vec3 Pym = neighbourPos(vUV - vec2(0.0, uTexel.y), depth);

    vec3 dx = minDiff(P, Px, Pxm);
    vec3 dy = minDiff(P, Py, Pym);
    vec3 N = normalize(cross(dx, dy));
    if (N.z < 0.0) N = -N;              // always face the camera

    vec3 V = normalize(-P);
    vec3 Nworld = normalize(mat3(uInvView) * N);
    vec3 Vworld = normalize(mat3(uInvView) * V);

    float thickness = texture(uThickness, vUV).r * uThicknessScale;

    // REFRACTION. Offset the background sample along the surface normal,
    // by more where the fluid is thicker. Clamped so a near-silhouette
    // normal cannot fling the sample across the frame.
    vec2 offset = N.xy * uRefract * clamp(thickness * 4.0, 0.0, 1.0);
    offset = clamp(offset, vec2(-0.08), vec2(0.08));
    vec3 refracted = texture(uSceneColor, clamp(vUV + offset, vec2(0.0), vec2(1.0))).rgb;

    // BEER-LAMBERT. Colour comes from absorption over the optical path,
    // NOT from a diffuse albedo: deep volume tints strongly, thin sheets
    // stay nearly clear. This is why a splash sheet and a settled pool
    // look like the same fluid at different depths rather than like two
    // different materials.
    //
    // TWO SEPARATE TERMS, and keeping them separate matters. The first is
    // the background seen THROUGH the fluid, attenuated per channel by
    // exp(-sigma_a * d). The second is light scattered back out of the
    // volume toward the eye, which saturates with optical depth.
    //
    // The scattering term uses a SCALAR optical depth, not the per-channel
    // (1 - transmittance). Using the per-channel complement is the obvious
    // thing to write and it is wrong: (1 - transmittance) is largest in
    // whichever channel is absorbed MOST, so the emergent colour comes out
    // as the complement of the fluid's absorption spectrum. Water, whose
    // absorption is strongest in red, then renders brown. (Observed
    // directly -- the first working frames of the dam break came out the
    // colour of rust.)
    vec3 transmittance = exp(-uAbsorption * thickness);
    float opticalDepth = 1.0 - exp(-uScatter * thickness);
    vec3 body = refracted * transmittance + uTint * opticalDepth;

    // REFLECTION + FRESNEL (Schlick). F0 = 0.02 is water against air.
    vec3 R = reflect(-Vworld, Nworld);
    vec3 reflected = environmentColor(R);
    float cosTheta = clamp(dot(Nworld, Vworld), 0.0, 1.0);
    float F = 0.02 + 0.98 * pow(1.0 - cosTheta, 5.0);

    vec3 color = mix(body, reflected, F);

    // SPECULAR from the three-point rig, in world space so the highlight
    // sweeps across the surface as the camera orbits rather than staying
    // pinned to it.
    //
    // Each lobe is modulated by Fresnel at its own HALF-VECTOR, not by the
    // view-normal Fresnel F used for the reflection mix. Reusing F here is
    // an easy mistake with a very visible consequence: F is 0.02 for water
    // at normal incidence, so a face-on surface would show essentially no
    // highlight at all and the fluid would read as a flat coloured shape
    // with no sense of a surface. A highlight is the reflection of the
    // light in a microfacet whose normal is H, so H is what its Fresnel
    // takes.
    vec3 spec = vec3(0.0);
    vec3 L1 = normalize(-uKeyDir);
    vec3 L2 = normalize(-uFillDir);
    vec3 L3 = normalize(-uRimDir);
    vec3 H1 = normalize(L1 + Vworld);
    vec3 H2 = normalize(L2 + Vworld);
    vec3 H3 = normalize(L3 + Vworld);
    float f1 = 0.02 + 0.98 * pow(1.0 - clamp(dot(H1, Vworld), 0.0, 1.0), 5.0);
    float f2 = 0.02 + 0.98 * pow(1.0 - clamp(dot(H2, Vworld), 0.0, 1.0), 5.0);
    float f3 = 0.02 + 0.98 * pow(1.0 - clamp(dot(H3, Vworld), 0.0, 1.0), 5.0);
    spec += uKeyColor  * uKeyIntensity  * f1 * pow(max(dot(Nworld, H1), 0.0), uSpecPower);
    spec += uFillColor * uFillIntensity * f2 * pow(max(dot(Nworld, H2), 0.0), uSpecPower * 0.5);
    spec += uRimColor  * uRimIntensity  * f3 * pow(max(dot(Nworld, H3), 0.0), uSpecPower * 0.25);
    color += spec * uSpecIntensity;

    FragColor = vec4(color, 1.0);
}
)GLSL";

} // namespace

FluidRenderer::FluidRenderer(int width, int height)
    : width_(std::max(1, width)), height_(std::max(1, height)) {
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    depthShader_        = std::make_unique<Shader>(kDepthVS, kDepthFS);
    blurShader_         = std::make_unique<Shader>(kFullScreenVS, kBlurFS);
    thicknessShader_    = std::make_unique<Shader>(kThicknessVS, kThicknessFS);
    thicknessBlurShader_= std::make_unique<Shader>(kFullScreenVS, kThicknessBlurFS);
    compositeShader_    = std::make_unique<Shader>(kFullScreenVS, kCompositeFS);

    depthTarget_   = std::make_unique<OffscreenTarget>(width_, height_, OffscreenTarget::Format::R32F, true);
    smoothA_       = std::make_unique<OffscreenTarget>(width_, height_, OffscreenTarget::Format::R32F, false);
    smoothB_       = std::make_unique<OffscreenTarget>(width_, height_, OffscreenTarget::Format::R32F, false);
    thickness_     = std::make_unique<OffscreenTarget>(width_, height_, OffscreenTarget::Format::R32F, false);
    thicknessBlur_ = std::make_unique<OffscreenTarget>(width_, height_, OffscreenTarget::Format::R32F, false);
}

void FluidRenderer::resize(int width, int height) {
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    depthTarget_->resize(width_, height_);
    smoothA_->resize(width_, height_);
    smoothB_->resize(width_, height_);
    thickness_->resize(width_, height_);
    thicknessBlur_->resize(width_, height_);
}

void FluidRenderer::ensureCapacity(size_t count) {
    if (count <= capacity_) return;
    // Grow with headroom: emitters make the particle count rise during a
    // run, and reallocating the GPU buffer on the exact step each new
    // particle appears would stall the pipeline every frame.
    capacity_ = std::max<size_t>(count * 2, 4096);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(capacity_ * 3 * sizeof(float)),
                  nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE_V, static_cast<GLsizei>(3 * sizeof(float)),
                           reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

void FluidRenderer::updateParticles(const std::vector<Particle>& particles, size_t firstFluid) {
    particleCount_ = particles.size() > firstFluid ? particles.size() - firstFluid : 0;
    ensureCapacity(particleCount_);
    cpu_.clear();
    cpu_.reserve(particleCount_ * 3);
    for (size_t i = firstFluid; i < particles.size(); ++i) {
        cpu_.push_back(particles[i].position.x);
        cpu_.push_back(particles[i].position.y);
        cpu_.push_back(particles[i].position.z);
    }
    if (cpu_.empty()) return;
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                     static_cast<GLsizeiptr>(cpu_.size() * sizeof(float)), cpu_.data());
}

void FluidRenderer::render(const OffscreenTarget& scene, OffscreenTarget& output,
                            const glm::mat4& view, const glm::mat4& proj,
                            float nearPlane, float farPlane, float fovYRadians,
                            const LightingSpec& lighting, const Params& params) {
    const float pointScale =
        static_cast<float>(height_) / (2.0f * std::tan(fovYRadians * 0.5f));
    // gl_PointSize has an implementation-defined ceiling. 255 is the
    // conservative floor across GL 3.3 core implementations; clamping to
    // it means a camera very close to the fluid under-sizes the impostors
    // and can open holes, rather than the driver silently clamping to
    // something smaller. Documented in docs/rendering.md under known
    // limitations, with "pull the camera back" as the remedy.
    constexpr float maxPointSize = 255.0f;
    const glm::vec2 texel(1.0f / static_cast<float>(width_), 1.0f / static_cast<float>(height_));

    // ---- Pass 1: depth ----
    depthTarget_->bind();
    // The colour attachment is cleared to the background sentinel, not to
    // zero: zero is a perfectly valid eye depth (the camera's own plane),
    // so clearing to it would make the whole background read as fluid
    // pressed against the lens.
    glClearColor(kBackgroundDepth, kBackgroundDepth, kBackgroundDepth, kBackgroundDepth);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE_V);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_BLEND);
    glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);

    if (particleCount_ > 0) {
        depthShader_->use();
        depthShader_->setMat4("uView", glm::value_ptr(view));
        depthShader_->setMat4("uProj", glm::value_ptr(proj));
        depthShader_->setFloat("uPointScale", pointScale);
        depthShader_->setFloat("uRadius", params.particleRadius);
        depthShader_->setFloat("uMaxPointSize", maxPointSize);
        glBindVertexArray(vao_);
        glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(particleCount_));
        glBindVertexArray(0);
    }

    // ---- Pass 2: separable bilateral smoothing, ping-ponged ----
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE_V);
    blurShader_->use();
    blurShader_->setVec2("uTexel", texel.x, texel.y);
    blurShader_->setFloat("uRadiusPixels", params.blurRadiusPixels);
    blurShader_->setFloat("uDepthFalloff", params.depthFalloff);
    blurShader_->setFloat("uBackground", kBackgroundDepth * 0.5f);

    unsigned int src = depthTarget_->colorTexture();
    for (int i = 0; i < std::max(1, params.smoothIterations); ++i) {
        smoothA_->bind();
        blurShader_->use();
        blurShader_->setVec2("uDir", 1.0f, 0.0f);
        blurShader_->setTexture("uDepth", 0, src);
        quad_.draw();

        smoothB_->bind();
        blurShader_->use();
        blurShader_->setVec2("uDir", 0.0f, 1.0f);
        blurShader_->setTexture("uDepth", 0, smoothA_->colorTexture());
        quad_.draw();

        src = smoothB_->colorTexture();
    }

    // ---- Pass 4: thickness ----
    thickness_->bind();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    // Additive with the depth test OFF, on purpose: every particle along
    // the ray must contribute, including the ones behind the front
    // surface. That is what makes this a path length rather than a
    // silhouette.
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    if (particleCount_ > 0) {
        thicknessShader_->use();
        thicknessShader_->setMat4("uView", glm::value_ptr(view));
        thicknessShader_->setMat4("uProj", glm::value_ptr(proj));
        thicknessShader_->setFloat("uPointScale", pointScale);
        thicknessShader_->setFloat("uRadius", params.particleRadius);
        thicknessShader_->setFloat("uMaxPointSize", maxPointSize);
        glBindVertexArray(vao_);
        glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(particleCount_));
        glBindVertexArray(0);
    }
    glDisable(GL_BLEND);

    thicknessBlur_->bind();
    thicknessBlurShader_->use();
    thicknessBlurShader_->setVec2("uTexel", texel.x, texel.y);
    thicknessBlurShader_->setTexture("uThickness", 0, thickness_->colorTexture());
    quad_.draw();

    // ---- Pass 5: composite ----
    output.bind();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    const glm::mat4 invProj = glm::inverse(proj);
    const glm::mat4 invView = glm::inverse(view);

    compositeShader_->use();
    compositeShader_->setMat4("uInvProj", glm::value_ptr(invProj));
    compositeShader_->setMat4("uInvView", glm::value_ptr(invView));
    compositeShader_->setVec2("uTexel", texel.x, texel.y);
    compositeShader_->setFloat("uBackground", kBackgroundDepth * 0.5f);
    compositeShader_->setFloat("uNear", nearPlane);
    compositeShader_->setFloat("uFar", farPlane);
    compositeShader_->setVec3("uAbsorption", params.absorption.x, params.absorption.y, params.absorption.z);
    compositeShader_->setVec3("uTint", params.tint.x, params.tint.y, params.tint.z);
    compositeShader_->setFloat("uRefract", params.refractionStrength);
    compositeShader_->setFloat("uScatter", params.scatterCoefficient);
    compositeShader_->setFloat("uThicknessScale", params.thicknessScale);
    compositeShader_->setFloat("uSpecPower", params.specularPower);
    compositeShader_->setFloat("uSpecIntensity", params.specularIntensity);

    const glm::vec3 key = glm::normalize(lighting.keyDirection);
    const glm::vec3 fill = glm::normalize(lighting.fillDirection);
    const glm::vec3 rim = glm::normalize(lighting.rimDirection);
    compositeShader_->setVec3("uKeyDir", key.x, key.y, key.z);
    compositeShader_->setVec3("uKeyColor", lighting.keyColor.x, lighting.keyColor.y, lighting.keyColor.z);
    compositeShader_->setFloat("uKeyIntensity", lighting.keyIntensity);
    compositeShader_->setVec3("uFillDir", fill.x, fill.y, fill.z);
    compositeShader_->setVec3("uFillColor", lighting.fillColor.x, lighting.fillColor.y, lighting.fillColor.z);
    compositeShader_->setFloat("uFillIntensity", lighting.fillIntensity);
    compositeShader_->setVec3("uRimDir", rim.x, rim.y, rim.z);
    compositeShader_->setVec3("uRimColor", lighting.rimColor.x, lighting.rimColor.y, lighting.rimColor.z);
    compositeShader_->setFloat("uRimIntensity", lighting.rimIntensity);

    compositeShader_->setTexture("uDepth", 0, src);
    compositeShader_->setTexture("uThickness", 1, thicknessBlur_->colorTexture());
    compositeShader_->setTexture("uSceneColor", 2, scene.colorTexture());
    compositeShader_->setTexture("uSceneDepth", 3, scene.depthTexture());
    quad_.draw();

    glActiveTexture(GL_TEXTURE0);
    glDepthMask(GL_TRUE_V);
}

} // namespace aquasph
