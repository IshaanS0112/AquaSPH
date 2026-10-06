#pragma once
#include <glm/glm.hpp>

namespace aquasph {

// Mouse-orbit camera around a fixed target point.
class OrbitCamera {
public:
    OrbitCamera(const glm::vec3& target, float distance,
                 float yawDeg = 45.0f, float pitchDeg = 25.0f, float fovDeg = 45.0f);

    // Degrees, not radians -- these are driven directly by raw mouse pixel deltas in
    // render_main.cpp, so degrees keeps the sensitivity tuning constant there readable (e.g.
    // "0.3 deg/pixel") instead of a tiny radians constant.
    void orbit(float deltaYawDeg, float deltaPitchDeg);
    void zoom(float deltaDistance);

    // Absolute yaw, for the scenario's slow constant orbit.
    void setYaw(float yawDeg);

    // Near/far are scenario-dependent: a 9 cm droplet scene and a 3 m flume cannot share one
    // pair without either clipping the near geometry or throwing away most of the depth
    // buffer's precision.
    void setClipPlanes(float nearPlane, float farPlane);

    glm::mat4 viewMatrix() const;
    glm::mat4 projectionMatrix(float aspectRatio) const;

    // World-space eye position, needed by the floor shader's distance
    // fade and by any world-space lighting term.
    glm::vec3 position() const;

    float fovDeg() const { return fovDeg_; }
    float nearPlane() const { return near_; }
    float farPlane() const { return far_; }

private:
    glm::vec3 target_;
    float distance_;
    float yawDeg_ = 45.0f;
    float pitchDeg_ = 25.0f;
    float fovDeg_ = 45.0f;
    float near_ = 0.01f;
    float far_ = 100.0f;
};

} // namespace aquasph
