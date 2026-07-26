#pragma once
#include <glm/glm.hpp>

namespace aquasph {

// Monaghan cubic B-spline kernel (M4), parameterized directly by the
// smoothing radius h such that the kernel has *compact support at r = h*
// (i.e. W(r,h) = 0 for r >= h; the piecewise break is at r = h/2).
//
// TWO BUGS IN THE ORIGINAL SPEC'S KERNEL FORMULA, FOUND BY ACTUALLY
// VERIFYING THIS AGAINST UNIT TESTS (see tests/test_kernel.cpp) --
// documented here since "why the cubic spline kernel" is an explicit
// interview talking point for this project, and getting either of these
// wrong live would be worse than not building this project at all.
//
// Bug 1 -- wrong dimensionality. The spec quotes sigma = 10/(7*pi*h^2)
// for "3D". That constant has units of 1/length^2 -- it's the 2D
// normalization constant, mislabeled.
//
// Bug 2 -- discontinuous kernel. The spec's two polynomial pieces,
//     (2/3 - q^2 + 0.5q^3)  for 0<=q<0.5   and   (1/6)(2-q)^3  for 0.5<=q<1
// (q = r/h), do not agree at the q=0.5 boundary: the first evaluates to
// 0.4792 there and the second to 0.5625. That 0.5-piece is exactly (2/3)
// times the *unscaled* standard cubic spline (the one whose natural
// support is q in [0,2], break at q=1) -- i.e. the breakpoints were
// halved (h -> h/2, 2h -> h) without rescaling the polynomial itself to
// match, which breaks continuity. A discontinuous kernel produces
// discontinuous (hence noisy/unstable) density and force fields.
//
// This implementation instead substitutes Q = 2*(r/h) into the genuine
// standard cubic spline f_std(Q) = 1-1.5Q^2+0.75Q^3 (Q<1), 0.25(2-Q)^3
// (1<=Q<2), which is the textbook-correct way to compress its support
// from [0,2h] down to [0,h]. That gives, in q = r/h:
//     f(q) = 1 - 6q^2 + 6q^3,   0 <= q < 0.5
//     f(q) = 2*(1-q)^3,          0.5 <= q < 1
// which is continuous (both pieces equal 0.25 at q=0.5) and in fact C1
// continuous (both derivatives equal -1.5 at q=0.5, and the value AND
// slope both go to zero at q=1) -- exactly the shape a compact-support
// spline should have.
//
// NORMALIZATION CONSTANT: solving sigma * integral_0^h W(r) 4*pi*r^2 dr = 1
// for the f(q) above gives integral_0^1 f(q) q^2 dq = 1/32 exactly, so
//     sigma_3D = 8 / (pi * h^3)
// (a clean, textbook-recognizable constant -- corroborating that this is
// the correct fix, vs. the original's messy, non-standard values).
// Verified numerically (Riemann sum) in tests/test_kernel.cpp.
class CubicSplineKernel {
public:
    explicit CubicSplineKernel(float h);

    float h() const { return h_; }
    float supportRadius() const { return h_; }

    // Kernel value for a pairwise separation distance r (r must be >= 0).
    float W(float r) const;

    // Gradient of W with respect to particle i's position, for separation
    // vector rij = r_i - r_j. Returns (0,0,0) when |rij| ~ 0 (undefined
    // direction for coincident particles; the true gradient magnitude is
    // 0 there anyway since W has a smooth maximum at r=0).
    glm::vec3 gradW(const glm::vec3& rij) const;

private:
    float h_;
    float sigma_;

    // Scalar radial derivative dW/dr at distance r, before it's turned
    // into a vector via the r_ij direction in gradW().
    float dWdr(float r) const;
};

} // namespace aquasph
