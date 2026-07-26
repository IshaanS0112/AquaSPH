#include "Camera.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace aquasph {

namespace {
constexpr float kMinPitchDeg = -89.0f;
constexpr float kMaxPitchDeg = 89.0f;
constexpr float kMinDistance = 0.1f;
constexpr float kMaxDistance = 50.0f;
}

OrbitCamera::OrbitCamera(const glm::vec3& target, float distance)
    : target_(target), distance_(std::clamp(distance, kMinDistance, kMaxDistance)) {}

void OrbitCamera::orbit(float deltaYawDeg, float deltaPitchDeg) {
    yawDeg_ += deltaYawDeg;
    // Clamped, not wrapped: past +-89 degrees the eye position and the
    // "up" vector used by glm::lookAt become nearly parallel, which is
    // the classic gimbal-lock-adjacent glitch (the view snaps/flips
    // instead of continuing to orbit smoothly).
    pitchDeg_ = std::clamp(pitchDeg_ + deltaPitchDeg, kMinPitchDeg, kMaxPitchDeg);
}

void OrbitCamera::zoom(float deltaDistance) {
    distance_ = std::clamp(distance_ + deltaDistance, kMinDistance, kMaxDistance);
}

glm::mat4 OrbitCamera::viewMatrix() const {
    const float yaw = glm::radians(yawDeg_);
    const float pitch = glm::radians(pitchDeg_);

    // Standard spherical-to-Cartesian eye position around target_.
    const glm::vec3 offset(
        distance_ * std::cos(pitch) * std::sin(yaw),
        distance_ * std::sin(pitch),
        distance_ * std::cos(pitch) * std::cos(yaw));

    const glm::vec3 eye = target_ + offset;
    return glm::lookAt(eye, target_, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 OrbitCamera::projectionMatrix(float aspectRatio) const {
    return glm::perspective(glm::radians(45.0f), aspectRatio, 0.01f, 100.0f);
}

} // namespace aquasph
