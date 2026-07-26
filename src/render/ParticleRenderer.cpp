#include "ParticleRenderer.hpp"
#include "GLLoader.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>

namespace aquasph {

using namespace gl;

namespace {

// #version 330 core matches this project's context-creation request
// (GLFW_CONTEXT_VERSION_MAJOR/MINOR = 3.3, core profile) in
// render_main.cpp -- must stay in sync with that request or context
// creation will succeed while shader compilation fails (a real, easy-to-
// hit mismatch bug if the two are ever changed independently).
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
    // Two-stop colormap: still water -> deep blue, fast-moving particles
    // (e.g. the floor-impact spike documented in docs/architecture.md)
    // -> white. Deliberately not a perceptually-uniform colormap
    // (viridis etc.) -- overkill for a debug/demo visualizer where the
    // only thing that matters is "which particles are moving fast".
    vec3 slow = vec3(0.10, 0.25, 0.85);
    vec3 fast = vec3(1.00, 1.00, 1.00);
    vec3 color = mix(slow, fast, clamp(vSpeedNorm, 0.0, 1.0));

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
    : maxParticles_(maxParticles), shader_(kVertexSrc, kFragmentSrc) {
    cpuBuffer_.reserve(maxParticles_ * 4);

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);

    // Allocated once at the run's fixed particle count; every later
    // frame reuses this same GPU allocation via glBufferSubData rather
    // than reallocating (glBufferData) every frame -- particle count
    // never changes mid-run, so there's nothing to grow into.
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(maxParticles_ * 4 * sizeof(float)),
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
                                        float speedForFullColor) {
    particleCount_ = std::min(particles.size(), maxParticles_);
    cpuBuffer_.clear();

    const float denom = speedForFullColor > 1e-6f ? speedForFullColor : 1.0f;
    for (size_t i = 0; i < particleCount_; ++i) {
        const Particle& p = particles[i];
        cpuBuffer_.push_back(p.position.x);
        cpuBuffer_.push_back(p.position.y);
        cpuBuffer_.push_back(p.position.z);
        const float speed = glm::length(p.velocity);
        cpuBuffer_.push_back(std::clamp(speed / denom, 0.0f, 1.0f));
    }

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
