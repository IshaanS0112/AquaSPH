#pragma once
#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include "OffscreenTarget.hpp"
#include "Shader.hpp"
#include "../core/Particle.hpp"
#include "../scene/Scenario.hpp"

namespace aquasph {

// Everything in the frame that is not the fluid: background gradient, matte floor with a scale
// grid and a soft contact shadow, the domain wireframe, and obstacles drawn as opaque solids.
class Environment {
public:
    Environment();

    // Renders background, floor, grid, wireframe and obstacles into `target`, which the fluid
    // composite then reads as its refraction source and occlusion depth.
    void render(OffscreenTarget& target,
                 const Scenario& scenario,
                 const glm::mat4& view, const glm::mat4& proj,
                 const glm::vec3& cameraPos,
                 float pointScale, float obstacleRadius);

    // Top-down orthographic thickness of the fluid, used by the floor shader as a soft contact
    // shadow.
    void updateContactShadow(const std::vector<Particle>& particles, size_t firstFluid,
                              const Scenario& scenario, float particleRadius);

    // Obstacle boundary particles, uploaded once (or whenever a moving obstacle changes them)
    // and drawn as matte impostor spheres.
    void updateObstacles(const std::vector<Particle>& particles,
                          size_t wallCount, size_t boundaryCount);

private:
    void drawBackground(const Scenario& scenario);
    void drawFloor(const Scenario& scenario, const glm::mat4& view, const glm::mat4& proj,
                    const glm::vec3& cameraPos);
    void drawWireframe(const Scenario& scenario, const glm::mat4& view, const glm::mat4& proj);
    void drawObstacles(const Scenario& scenario, const glm::mat4& view, const glm::mat4& proj,
                        float pointScale, float obstacleRadius);

    std::unique_ptr<Shader> backgroundShader_;
    std::unique_ptr<Shader> floorShader_;
    std::unique_ptr<Shader> lineShader_;
    std::unique_ptr<Shader> obstacleShader_;
    std::unique_ptr<Shader> shadowShader_;
    std::unique_ptr<OffscreenTarget> shadowMap_;

    FullScreenTriangle quad_;
    gl::GLuint floorVao_ = 0, floorVbo_ = 0;
    gl::GLuint lineVao_ = 0, lineVbo_ = 0;
    gl::GLuint obstacleVao_ = 0, obstacleVbo_ = 0;
    gl::GLuint fluidVao_ = 0, fluidVbo_ = 0;
    size_t obstacleCount_ = 0;
    size_t fluidCapacity_ = 0;
    size_t fluidCount_ = 0;
    bool haveShadow_ = false;
};

} // namespace aquasph
