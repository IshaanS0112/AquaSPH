#pragma once
#include <vector>
#include "Particle.hpp"
#include "../io/ConfigLoader.hpp"

namespace aquasph {

// Fills a cube of particles in one corner of the domain on a regular
// lattice (dense enough to give interior particles ~30+ neighbors within
// the kernel support radius h). The block collapses under gravity once
// the simulation starts: the classic SPH "dam break" validation case.
//
// Shared by both entry points (headless `aquasph` and the
// `aquasph_view` renderer) so the carefully-derived mass/spacing logic
// below -- and the bug history behind it -- exists in exactly one place.
// See the detailed rationale in DamBreakInit.cpp.
std::vector<Particle> initializeDamBreak(const Config& cfg, int targetCount);

} // namespace aquasph
