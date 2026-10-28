#include "SPHKernel.hpp"
#include "Constants.hpp"

namespace aquasph {

CubicSplineKernel::CubicSplineKernel(float h) : h_(h) {
    sigma_ = 8.0f / (constants::kPi * h_ * h_ * h_);
}

float CubicSplineKernel::W(float r) const {
    const float q = r / h_;
    if (q >= 1.0f) return 0.0f;
    if (q < 0.5f) {
        return sigma_ * (1.0f - 6.0f * q * q + 6.0f * q * q * q);
    }
    const float t = 1.0f - q;
    return sigma_ * (2.0f * t * t * t);
}

float CubicSplineKernel::dWdr(float r) const {
    const float q = r / h_;
    if (q >= 1.0f) return 0.0f;
    float dfdq;
    if (q < 0.5f) {
        dfdq = -12.0f * q + 18.0f * q * q;
    } else {
        const float t = 1.0f - q;
        dfdq = -6.0f * t * t;
    }
    return sigma_ * dfdq / h_;
}

glm::vec3 CubicSplineKernel::gradW(const glm::vec3& rij) const {
    const float r = glm::length(rij);
    if (r < constants::kEpsilon) {
        return glm::vec3(0.0f);
    }
    return dWdr(r) * (rij / r);
}

} // namespace aquasph
