#pragma once
#include <string>

namespace aquasph {

// Simulation resolution and visual quality are different problems, and
// conflating them is why this exists as its own concept.
//
// A screen-space fluid surface reconstructed from ~8,000 particles reads
// as lumpy no matter how good the shader is -- the surface simply is not
// there to find. Convincing results want a few hundred thousand. But
// measured throughput on the reference machine is a few frames per second
// at 50k particles on four threads, so showcase resolution and real-time
// interaction are not simultaneously available. Rather than pretend
// otherwise, they are separate presets and every published result states
// which one it used.
//
// The scale multiplies BOTH h and the particle spacing, so the ratio
// between them -- and therefore the number of neighbours each particle
// sees, and therefore the accuracy of every kernel sum -- is identical
// across presets. Changing resolution changes how finely the fluid is
// sampled, never how well-conditioned the discretisation is.
//
// The particle counts these produce depend on the scenario's geometry and
// are printed by every run rather than promised here.
enum class Quality { Low, Medium, High };

inline const char* qualityName(Quality q) {
    switch (q) {
        case Quality::Low: return "low";
        case Quality::Medium: return "medium";
        case Quality::High: return "high";
    }
    return "medium";
}

// Resolution scale applied to h and spacing. Particle count scales as
// roughly the inverse cube: low is ~5.8x fewer than medium, high is ~6x
// more.
inline float qualityScale(Quality q) {
    switch (q) {
        case Quality::Low: return 1.8f;
        case Quality::Medium: return 1.0f;
        case Quality::High: return 0.55f;
    }
    return 1.0f;
}

// Framebuffer edge length for offscreen capture, and the number of
// bilateral-filter iterations applied to the depth buffer. More particles
// support -- and need -- more smoothing passes to close the gaps between
// them without erasing surface detail.
inline int qualityCaptureSize(Quality q) {
    switch (q) {
        case Quality::Low: return 720;
        case Quality::Medium: return 1080;
        case Quality::High: return 1600;
    }
    return 1080;
}

inline int qualitySmoothIterations(Quality q) {
    switch (q) {
        case Quality::Low: return 2;
        case Quality::Medium: return 4;
        case Quality::High: return 6;
    }
    return 4;
}

inline bool parseQuality(const std::string& s, Quality& out) {
    if (s == "low") { out = Quality::Low; return true; }
    if (s == "medium") { out = Quality::Medium; return true; }
    if (s == "high") { out = Quality::High; return true; }
    return false;
}

} // namespace aquasph
