#pragma once

namespace aquasph {

// Centralized physical/numerical constants so magic numbers don't get
// scattered across the codebase. Every value here documents where it
// comes from and why it has this value (see project notes: "Document
// every magic number").
namespace constants {

constexpr float kPi = 3.14159265358979323846f;

// Earth-surface gravitational acceleration, m/s^2.
constexpr float kGravityMagnitude = 9.81f;

// Guards divisions by |r_ij| when two particles are numerically
// coincident (self-pair, or two particles integrated to the same point).
constexpr float kEpsilon = 1e-8f;

} // namespace constants
} // namespace aquasph
