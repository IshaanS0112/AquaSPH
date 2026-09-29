#include "ParticleRenderer.hpp"
#include "GLLoader.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>

namespace aquasph {

using namespace gl;

namespace {

// #version 330 core matches this project's context-creation request
// (GLFW_CONTEXT_VERSION_MAJOR/MINOR = 3.3, core profile) in render_main.cpp.
const char* kVertexSrc = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in float aSpeedNorm;

uniform mat4 uView;
uniform mat4 uProj;
uniform float uPointSize;

out float vSpeedNorm;

void main() {
    gl_Position = uProj * uView * vec4(aPos, 1.0);
    vSpeedNorm = aSpeedNorm;
    gl_PointSize = uPointSize;
}
)GLSL";

const char* kFragmentSrc = R"GLSL(
#version 330 core
in float vSpeedNorm;
out vec4 FragColor;

void main() {
    // Three-stop ramp within the project's one palette: deep teal at rest, through the fluid's
    // own cyan, to a pale highlight at the scenario's reference speed.
    vec3 slow = vec3(0.055, 0.180, 0.235);
    vec3 mid  = vec3(0.180, 0.560, 0.640);
    vec3 fast = vec3(0.850, 0.960, 0.980);
    float t = clamp(vSpeedNorm, 0.0, 1.0);
    vec3 color = t < 0.5 ? mix(slow, mid, t * 2.0) : mix(mid, fast, (t - 0.5) * 2.0);

    // GL_POINTS defaults to a square sprite; discard the corners so
    // particles read as circles/droplets instead of little tiles.
    vec2 centered = gl_PointCoord - vec2(0.5);
    if (dot(centered, centered) > 0.25) {
        discard;
    }

    FragColor = vec4(color, 1.0);
}
)GLSL";

} // namespace

ParticleRenderer::ParticleRenderer(size_t maxParticles)
    : shader_(kVertexSrc, kFragmentSrc) {
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    ensureCapacity(maxParticles);
}

// GROWS ON DEMAND. v1 allocated once at "the run's fixed particle count" because the count
// could not change; emitters made that false.
void ParticleRenderer::ensureCapacity(size_t count) {
    if (count <= capacity_) return;
    capacity_ = std::max<size_t>(count * 2, 4096);
    cpuBuffer_.reserve(capacity_ * 4);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(capacity_ * 4 * sizeof(float)),
                 nullptr, GL_DYNAMIC_DRAW);

    const GLsizei stride = static_cast<GLsizei>(4 * sizeof(float));
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE_V, stride,
                           reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE_V, stride,
                           reinterpret_cast<const void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

ParticleRenderer::~ParticleRenderer() {
    glDeleteBuffers(1, &vbo_);
    glDeleteVertexArrays(1, &vao_);
}

void ParticleRenderer::updateParticles(const std::vector<Particle>& particles,
                                        size_t firstFluid, float referenceSpeed) {
    particleCount_ = particles.size() > firstFluid ? particles.size() - firstFluid : 0;
    ensureCapacity(particleCount_);
    cpuBuffer_.clear();

    const float denom = referenceSpeed > 1e-6f ? referenceSpeed : 1.0f;
    for (size_t i = firstFluid; i < particles.size(); ++i) {
        const Particle& p = particles[i];
        cpuBuffer_.push_back(p.position.x);
        cpuBuffer_.push_back(p.position.y);
        cpuBuffer_.push_back(p.position.z);
        const float speed = glm::length(p.velocity);
        cpuBuffer_.push_back(std::clamp(speed / denom, 0.0f, 1.0f));
    }
    if (cpuBuffer_.empty()) return;

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                     static_cast<GLsizeiptr>(cpuBuffer_.size() * sizeof(float)),
                     cpuBuffer_.data());
}

void ParticleRenderer::draw(const glm::mat4& view, const glm::mat4& proj,
                             float pointSizePixels) const {
    shader_.use();
    shader_.setMat4("uView", glm::value_ptr(view));
    shader_.setMat4("uProj", glm::value_ptr(proj));
    shader_.setFloat("uPointSize", pointSizePixels);

    glBindVertexArray(vao_);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(particleCount_));
}

} // namespace aquasph
