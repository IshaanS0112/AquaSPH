#include "Environment.hpp"
#include "GLLoader.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>

namespace aquasph {

using namespace gl;

namespace {

constexpr int kShadowResolution = 512;

const char* kFullScreenVS = R"GLSL(
#version 330 core
out vec2 vUV;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// Background: a gentle vertical gradient plus a light vignette.
const char* kBackgroundFS = R"GLSL(
#version 330 core
in vec2 vUV;
uniform vec3 uTop;
uniform vec3 uBottom;
out vec4 FragColor;
void main() {
    vec3 c = mix(uBottom, uTop, smoothstep(0.0, 1.0, vUV.y));
    vec2 d = vUV - 0.5;
    float vignette = 1.0 - 0.35 * dot(d, d) * 2.0;
    FragColor = vec4(c * vignette, 1.0);
}
)GLSL";

const char* kFloorVS = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uView;
uniform mat4 uProj;
out vec3 vWorld;
void main() {
    vWorld = aPos;
    gl_Position = uProj * uView * vec4(aPos, 1.0);
}
)GLSL";

const char* kFloorFS = R"GLSL(
#version 330 core
in vec3 vWorld;

uniform vec3 uCameraPos;
uniform vec3 uBase;
uniform vec3 uGridColor;
uniform float uGridSpacing;
uniform float uFadeRadius;
uniform vec3 uDomainMin;
uniform vec3 uDomainMax;
uniform sampler2D uShadow;
uniform float uHasShadow;
uniform float uShadowStrength;

out vec4 FragColor;

// Analytically antialiased grid: line width is derived from the screen -space derivative of the
// world position, so lines stay one pixel wide at every distance instead of aliasing into noise
// near the horizon.
float gridLine(vec2 p, float spacing, float width) {
    vec2 g = abs(fract(p / spacing - 0.5) - 0.5) * spacing;
    vec2 fw = fwidth(p) * width;
    vec2 l = smoothstep(fw, vec2(0.0), g);
    return max(l.x, l.y);
}

void main() {
    vec2 p = vWorld.xz;
    float major = gridLine(p, uGridSpacing * 5.0, 1.6);
    float minor = gridLine(p, uGridSpacing, 1.0);

    vec3 color = uBase;
    color = mix(color, uGridColor, minor * 0.35);
    color = mix(color, uGridColor, major * 0.55);

    // Soft contact shadow from the top-down fluid thickness map.
    if (uHasShadow > 0.5) {
        vec2 uv = (p - uDomainMin.xz) / max(uDomainMax.xz - uDomainMin.xz, vec2(1e-4));
        if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) {
            float t = texture(uShadow, uv).r;
            float occl = 1.0 - exp(-t * 6.0);
            color *= mix(1.0, 1.0 - uShadowStrength, clamp(occl, 0.0, 1.0));
        }
    }

    // Fade to the background beyond the region of interest, so the floor
    // does not read as an infinite plane with a hard edge.
    float d = length(vWorld - vec3(uCameraPos.x, vWorld.y, uCameraPos.z));
    float fade = 1.0 - smoothstep(uFadeRadius * 0.45, uFadeRadius, d);
    FragColor = vec4(color, fade);
}
)GLSL";

const char* kLineVS = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uView;
uniform mat4 uProj;
void main() { gl_Position = uProj * uView * vec4(aPos, 1.0); }
)GLSL";

const char* kLineFS = R"GLSL(
#version 330 core
uniform vec4 uColor;
out vec4 FragColor;
void main() { FragColor = uColor; }
)GLSL";

// Obstacles: the same sphere-impostor trick as the fluid depth pass, but shaded opaquely.
const char* kObstacleVS = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uView;
uniform mat4 uProj;
uniform float uPointScale;
uniform float uRadius;
out vec3 vViewPos;
void main() {
    vec4 v = uView * vec4(aPos, 1.0);
    vViewPos = v.xyz;
    gl_Position = uProj * v;
    gl_PointSize = clamp(uPointScale * uRadius / max(-v.z, 1e-4), 1.0, 255.0);
}
)GLSL";

const char* kObstacleFS = R"GLSL(
#version 330 core
in vec3 vViewPos;
uniform mat4 uProj;
uniform mat4 uInvView;
uniform float uRadius;
uniform vec3 uColor;
uniform vec3 uKeyDir;  uniform vec3 uKeyColor;  uniform float uKeyIntensity;
uniform vec3 uFillDir; uniform vec3 uFillColor; uniform float uFillIntensity;
uniform vec3 uRimDir;  uniform vec3 uRimColor;  uniform float uRimIntensity;
out vec4 FragColor;
void main() {
    vec2 c = gl_PointCoord * 2.0 - 1.0;
    c.y = -c.y;
    float r2 = dot(c, c);
    if (r2 > 1.0) discard;
    vec3 n = vec3(c, sqrt(1.0 - r2));
    vec3 spherePos = vViewPos + n * uRadius;
    vec4 clip = uProj * vec4(spherePos, 1.0);
    gl_FragDepth = (clip.z / clip.w) * 0.5 + 0.5;

    vec3 N = normalize(mat3(uInvView) * n);
    // Matte Lambertian only -- no specular. Obstacles must read as solid
    // and inert so the eye goes to the fluid.
    float key  = max(dot(N, normalize(-uKeyDir)), 0.0)  * uKeyIntensity;
    float fill = max(dot(N, normalize(-uFillDir)), 0.0) * uFillIntensity;
    float rim  = max(dot(N, normalize(-uRimDir)), 0.0)  * uRimIntensity;
    vec3 lit = uColor * (0.18 + key * uKeyColor + fill * uFillColor + rim * uRimColor * 0.4);
    FragColor = vec4(lit, 1.0);
}
)GLSL";

// Top-down orthographic accumulation of fluid, for the contact shadow.
const char* kShadowVS = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform vec3 uDomainMin;
uniform vec3 uDomainMax;
uniform float uPointSize;
void main() {
    vec2 uv = (aPos.xz - uDomainMin.xz) / max(uDomainMax.xz - uDomainMin.xz, vec2(1e-4));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    gl_PointSize = uPointSize;
}
)GLSL";

const char* kShadowFS = R"GLSL(
#version 330 core
uniform float uWeight;
out float outValue;
void main() {
    vec2 c = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(c, c);
    if (r2 > 1.0) discard;
    outValue = uWeight * (1.0 - r2);
}
)GLSL";

} // namespace

Environment::Environment() {
    backgroundShader_ = std::make_unique<Shader>(kFullScreenVS, kBackgroundFS);
    floorShader_      = std::make_unique<Shader>(kFloorVS, kFloorFS);
    lineShader_       = std::make_unique<Shader>(kLineVS, kLineFS);
    obstacleShader_   = std::make_unique<Shader>(kObstacleVS, kObstacleFS);
    shadowShader_     = std::make_unique<Shader>(kShadowVS, kShadowFS);
    shadowMap_ = std::make_unique<OffscreenTarget>(kShadowResolution, kShadowResolution,
                                                    OffscreenTarget::Format::R32F, false);

    glGenVertexArrays(1, &floorVao_);
    glGenBuffers(1, &floorVbo_);
    glGenVertexArrays(1, &lineVao_);
    glGenBuffers(1, &lineVbo_);
    glGenVertexArrays(1, &obstacleVao_);
    glGenBuffers(1, &obstacleVbo_);
    glGenVertexArrays(1, &fluidVao_);
    glGenBuffers(1, &fluidVbo_);
}

void Environment::updateObstacles(const std::vector<Particle>& particles,
                                   size_t wallCount, size_t boundaryCount) {
    // Only obstacle boundary particles, never the tank walls: drawing the walls would wrap the
    // scene in an opaque box and hide the fluid entirely.
    obstacleCount_ = boundaryCount > wallCount ? boundaryCount - wallCount : 0;
    if (obstacleCount_ == 0) return;

    std::vector<float> data;
    data.reserve(obstacleCount_ * 3);
    for (size_t i = wallCount; i < boundaryCount; ++i) {
        data.push_back(particles[i].position.x);
        data.push_back(particles[i].position.y);
        data.push_back(particles[i].position.z);
    }

    glBindVertexArray(obstacleVao_);
    glBindBuffer(GL_ARRAY_BUFFER, obstacleVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)),
                  data.data(), GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE_V, static_cast<GLsizei>(3 * sizeof(float)),
                           reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

void Environment::updateContactShadow(const std::vector<Particle>& particles, size_t firstFluid,
                                       const Scenario& scenario, float particleRadius) {
    const size_t count = particles.size() > firstFluid ? particles.size() - firstFluid : 0;
    fluidCount_ = count;
    haveShadow_ = count > 0;
    if (count == 0) return;

    std::vector<float> data;
    data.reserve(count * 3);
    for (size_t i = firstFluid; i < particles.size(); ++i) {
        data.push_back(particles[i].position.x);
        data.push_back(particles[i].position.y);
        data.push_back(particles[i].position.z);
    }

    glBindVertexArray(fluidVao_);
    glBindBuffer(GL_ARRAY_BUFFER, fluidVbo_);
    if (count > fluidCapacity_) {
        fluidCapacity_ = std::max<size_t>(count * 2, 4096);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(fluidCapacity_ * 3 * sizeof(float)),
                      nullptr, GL_DYNAMIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE_V, static_cast<GLsizei>(3 * sizeof(float)),
                               reinterpret_cast<const void*>(0));
        glEnableVertexAttribArray(0);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                     static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.data());

    const glm::vec3 span = scenario.domain.max - scenario.domain.min;
    const float pixelsPerMetre = static_cast<float>(kShadowResolution) /
                                  std::max(std::max(span.x, span.z), 1.0e-4f);

    shadowMap_->bind();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);

    shadowShader_->use();
    shadowShader_->setVec3("uDomainMin", scenario.domain.min.x, scenario.domain.min.y, scenario.domain.min.z);
    shadowShader_->setVec3("uDomainMax", scenario.domain.max.x, scenario.domain.max.y, scenario.domain.max.z);
    shadowShader_->setFloat("uPointSize",
                             std::clamp(2.0f * particleRadius * pixelsPerMetre, 1.0f, 64.0f));
    shadowShader_->setFloat("uWeight", particleRadius);
    glBindVertexArray(fluidVao_);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(count));
    glBindVertexArray(0);

    glDisable(GL_BLEND);
}

void Environment::render(OffscreenTarget& target, const Scenario& scenario,
                          const glm::mat4& view, const glm::mat4& proj,
                          const glm::vec3& cameraPos,
                          float pointScale, float obstacleRadius) {
    target.bind();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    drawBackground(scenario);
    if (scenario.render.showFloor) drawFloor(scenario, view, proj, cameraPos);
    if (scenario.render.showDomainWireframe) drawWireframe(scenario, view, proj);
    drawObstacles(scenario, view, proj, pointScale, obstacleRadius);
}

void Environment::drawBackground(const Scenario&) {
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE_V);
    glDisable(GL_BLEND);
    backgroundShader_->use();
    // One restrained palette across every scenario: a neutral dark environment, so the fluid's
    // desaturated cyan reads the same way in a contact sheet regardless of which scenario a
    // frame came from.
    backgroundShader_->setVec3("uTop", 0.075f, 0.086f, 0.102f);
    backgroundShader_->setVec3("uBottom", 0.028f, 0.032f, 0.040f);
    quad_.draw();
    glDepthMask(GL_TRUE_V);
}

void Environment::drawFloor(const Scenario& scenario, const glm::mat4& view, const glm::mat4& proj,
                             const glm::vec3& cameraPos) {
    const glm::vec3 lo = scenario.domain.min;
    const glm::vec3 hi = scenario.domain.max;
    const glm::vec3 span = hi - lo;
    const float extent = std::max(span.x, span.z) * 6.0f;
    const glm::vec3 c((lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f);

    const float verts[] = {
        c.x - extent, c.y, c.z - extent,
        c.x + extent, c.y, c.z - extent,
        c.x - extent, c.y, c.z + extent,
        c.x + extent, c.y, c.z + extent,
    };

    glBindVertexArray(floorVao_);
    glBindBuffer(GL_ARRAY_BUFFER, floorVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE_V, static_cast<GLsizei>(3 * sizeof(float)),
                           reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(0);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    floorShader_->use();
    floorShader_->setMat4("uView", glm::value_ptr(view));
    floorShader_->setMat4("uProj", glm::value_ptr(proj));
    floorShader_->setVec3("uCameraPos", cameraPos.x, cameraPos.y, cameraPos.z);
    floorShader_->setVec3("uBase", 0.115f, 0.125f, 0.140f);
    floorShader_->setVec3("uGridColor", 0.30f, 0.36f, 0.40f);
    // Grid spacing chosen so the domain is a handful of major squares across, then rounded to a
    // readable number.
    const float raw = std::max(std::max(span.x, span.z) / 10.0f, 1.0e-3f);
    const float mag = std::pow(10.0f, std::floor(std::log10(raw)));
    const float nice = mag * (raw / mag < 2.0f ? 1.0f : (raw / mag < 5.0f ? 2.0f : 5.0f));
    floorShader_->setFloat("uGridSpacing", nice);
    floorShader_->setFloat("uFadeRadius", extent * 0.6f);
    floorShader_->setVec3("uDomainMin", lo.x, lo.y, lo.z);
    floorShader_->setVec3("uDomainMax", hi.x, hi.y, hi.z);
    floorShader_->setFloat("uHasShadow", haveShadow_ ? 1.0f : 0.0f);
    floorShader_->setFloat("uShadowStrength", 0.55f);
    floorShader_->setTexture("uShadow", 0, shadowMap_->colorTexture());

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

void Environment::drawWireframe(const Scenario& scenario, const glm::mat4& view,
                                 const glm::mat4& proj) {
    const glm::vec3 lo = scenario.domain.min;
    const glm::vec3 hi = scenario.domain.max;
    const glm::vec3 corners[8] = {
        {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z},
        {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z},
    };
    static const int edges[24] = {0,1, 1,2, 2,3, 3,0, 4,5, 5,6, 6,7, 7,4, 0,4, 1,5, 2,6, 3,7};

    std::vector<float> verts;
    verts.reserve(24 * 3);
    for (int e : edges) {
        verts.push_back(corners[e].x);
        verts.push_back(corners[e].y);
        verts.push_back(corners[e].z);
    }

    glBindVertexArray(lineVao_);
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                  verts.data(), GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE_V, static_cast<GLsizei>(3 * sizeof(float)),
                           reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(0);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    lineShader_->use();
    lineShader_->setMat4("uView", glm::value_ptr(view));
    lineShader_->setMat4("uProj", glm::value_ptr(proj));
    lineShader_->setVec4("uColor", 0.42f, 0.50f, 0.56f, 0.32f);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(verts.size() / 3));
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

void Environment::drawObstacles(const Scenario& scenario, const glm::mat4& view,
                                 const glm::mat4& proj,
                                 float pointScale, float obstacleRadius) {
    if (obstacleCount_ == 0) return;

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE_V);
    glDisable(GL_BLEND);
    glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);

    const glm::mat4 invView = glm::inverse(view);
    obstacleShader_->use();
    obstacleShader_->setMat4("uView", glm::value_ptr(view));
    obstacleShader_->setMat4("uProj", glm::value_ptr(proj));
    obstacleShader_->setMat4("uInvView", glm::value_ptr(invView));
    obstacleShader_->setFloat("uPointScale", pointScale);
    obstacleShader_->setFloat("uRadius", obstacleRadius);
    obstacleShader_->setVec3("uColor", 0.34f, 0.35f, 0.37f);

    const LightingSpec& L = scenario.lighting;
    obstacleShader_->setVec3("uKeyDir", L.keyDirection.x, L.keyDirection.y, L.keyDirection.z);
    obstacleShader_->setVec3("uKeyColor", L.keyColor.x, L.keyColor.y, L.keyColor.z);
    obstacleShader_->setFloat("uKeyIntensity", L.keyIntensity);
    obstacleShader_->setVec3("uFillDir", L.fillDirection.x, L.fillDirection.y, L.fillDirection.z);
    obstacleShader_->setVec3("uFillColor", L.fillColor.x, L.fillColor.y, L.fillColor.z);
    obstacleShader_->setFloat("uFillIntensity", L.fillIntensity);
    obstacleShader_->setVec3("uRimDir", L.rimDirection.x, L.rimDirection.y, L.rimDirection.z);
    obstacleShader_->setVec3("uRimColor", L.rimColor.x, L.rimColor.y, L.rimColor.z);
    obstacleShader_->setFloat("uRimIntensity", L.rimIntensity);

    glBindVertexArray(obstacleVao_);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(obstacleCount_));
    glBindVertexArray(0);
}

} // namespace aquasph
