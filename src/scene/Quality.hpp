#pragma once
#include <string>

namespace aquasph {

// Simulation resolution and visual quality are different problems, and conflating them is why
// this exists as its own concept.
enum class Quality { Low, Medium, High };

inline const char* qualityName(Quality q) {
    switch (q) {
        case Quality::Low: return "low";
        case Quality::Medium: return "medium";
        case Quality::High: return "high";
    }
    return "medium";
}

// Resolution scale applied to h and spacing.
inline float qualityScale(Quality q) {
    switch (q) {
        case Quality::Low: return 1.8f;
        case Quality::Medium: return 1.0f;
        case Quality::High: return 0.55f;
    }
    return 1.0f;
}

// Framebuffer edge length for offscreen capture, and the number of bilateral-filter iterations
// applied to the depth buffer.
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
