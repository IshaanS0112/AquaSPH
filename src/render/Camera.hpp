#pragma once
#include <glm/glm.hpp>

namespace aquasph {

// Mouse-orbit camera around a fixed target point. Adequate for
// inspecting a bounded dam-break domain (a few meters across at most) --
// deliberately not a general 6-DOF fly camera, since there's no reason
// to leave the vicinity of a small, fixed simulation box.
class OrbitCamera {
public:
    OrbitCamera(const glm::vec3& target, float distance);

    // Degrees, not radians -- these are driven directly by raw mouse
    // pixel deltas in render_main.cpp, so degrees keeps the sensitivity
    // tuning constant there readable (e.g. "0.3 deg/pixel") instead of
    // a tiny radians constant.
    void orbit(float deltaYawDeg, float deltaPitchDeg);
    void zoom(float deltaDistance);

    glm::mat4 viewMatrix() const;
    glm::mat4 projectionMatrix(float aspectRatio) const;

private:
    glm::vec3 target_;
    float distance_;
    float yawDeg_ = 45.0f;
    float pitchDeg_ = 25.0f;
};

} // namespace aquasph
