#pragma once
#include <glm/glm.hpp>

namespace aquasph {

// Monaghan cubic B-spline kernel (M4), parameterized directly by the smoothing radius h such
// that the kernel has *compact support at r = h* (i.e. W(r,h) = 0 for r >= h; the piecewise
// break is at r = h/2). Normalised by sigma = 8 / (pi h^3); tests/test_kernel.cpp checks it.
class CubicSplineKernel {
public:
    explicit CubicSplineKernel(float h);

    float h() const { return h_; }
    float supportRadius() const { return h_; }

    // Kernel value for a pairwise separation distance r (r must be >= 0).
    float W(float r) const;

    // Gradient of W with respect to r_i, for separation vector rij = r_i - r_j.
    glm::vec3 gradW(const glm::vec3& rij) const;

private:
    float h_;
    float sigma_;

    // Scalar radial derivative dW/dr at distance r, before it's turned
    // into a vector via the r_ij direction in gradW().
    float dWdr(float r) const;
};

} // namespace aquasph
