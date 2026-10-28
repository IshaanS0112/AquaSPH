#include "Camera.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace aquasph {

namespace {
constexpr float kMinPitchDeg = -89.0f;
constexpr float kMaxPitchDeg = 89.0f;
constexpr float kMinDistance = 0.1f;
constexpr float kMaxDistance = 500.0f;
}

OrbitCamera::OrbitCamera(const glm::vec3& target, float distance,
                          float yawDeg, float pitchDeg, float fovDeg)
    : target_(target), distance_(std::clamp(distance, kMinDistance, kMaxDistance)),
      yawDeg_(yawDeg), pitchDeg_(std::clamp(pitchDeg, kMinPitchDeg, kMaxPitchDeg)),
      fovDeg_(fovDeg) {}

void OrbitCamera::setYaw(float yawDeg) { yawDeg_ = yawDeg; }

void OrbitCamera::setClipPlanes(float nearPlane, float farPlane) {
    near_ = std::max(nearPlane, 1.0e-4f);
    far_ = std::max(farPlane, near_ * 10.0f);
}

glm::vec3 OrbitCamera::position() const {
    const float yaw = glm::radians(yawDeg_);
    const float pitch = glm::radians(pitchDeg_);
    return target_ + glm::vec3(distance_ * std::cos(pitch) * std::sin(yaw),
                                distance_ * std::sin(pitch),
                                distance_ * std::cos(pitch) * std::cos(yaw));
}

void OrbitCamera::orbit(float deltaYawDeg, float deltaPitchDeg) {
    yawDeg_ += deltaYawDeg;
    // Clamped, not wrapped: past +-89 degrees the eye position and the "up" vector used by
    // glm::lookAt become nearly parallel, which is the classic gimbal-lock-adjacent glitch (the
    // view snaps/flips instead of continuing to orbit smoothly).
    pitchDeg_ = std::clamp(pitchDeg_ + deltaPitchDeg, kMinPitchDeg, kMaxPitchDeg);
}

void OrbitCamera::zoom(float deltaDistance) {
    distance_ = std::clamp(distance_ + deltaDistance, kMinDistance, kMaxDistance);
}

glm::mat4 OrbitCamera::viewMatrix() const {
    // Standard spherical-to-Cartesian eye position around target_.
    return glm::lookAt(position(), target_, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 OrbitCamera::projectionMatrix(float aspectRatio) const {
    return glm::perspective(glm::radians(fovDeg_), aspectRatio, near_, far_);
}

} // namespace aquasph
