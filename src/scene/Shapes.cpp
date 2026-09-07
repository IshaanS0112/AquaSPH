#include "Shapes.hpp"
#include <algorithm>
#include <cmath>

namespace aquasph {

float HeightfieldSpec::heightAt(float x, float z) const {
    float y = base + slope.x * (x - origin.x) + slope.y * (z - origin.y);
    for (const Bump& b : bumps) {
        const float dx = x - b.center.x;
        const float dz = z - b.center.y;
        const float s2 = b.sigma * b.sigma;
        if (s2 <= 0.0f) continue;
        y += b.amplitude * std::exp(-(dx * dx + dz * dz) / (2.0f * s2));
    }
    return std::clamp(y, minHeight, maxHeight);
}

namespace {

bool inBox(const glm::vec3& p, const glm::vec3& lo, const glm::vec3& hi) {
    return p.x >= lo.x && p.x <= hi.x &&
           p.y >= lo.y && p.y <= hi.y &&
           p.z >= lo.z && p.z <= hi.z;
}

// Radial distance from the cylinder axis, and the coordinate along it.
void cylinderCoords(const Shape& s, const glm::vec3& p, float& radial, float& axial) {
    const glm::vec3 d = p - s.center;
    switch (s.axis) {
        case 0: radial = std::sqrt(d.y * d.y + d.z * d.z); axial = d.x; break;
        case 2: radial = std::sqrt(d.x * d.x + d.y * d.y); axial = d.z; break;
        default: radial = std::sqrt(d.x * d.x + d.z * d.z); axial = d.y; break;
    }
}

} // namespace

bool Shape::contains(const glm::vec3& p) const {
    switch (type) {
        case ShapeType::Box:
            return inBox(p, min, max);
        case ShapeType::Sphere:
            return glm::length(p - center) <= radius && inBox(p, min, max);
        case ShapeType::Cylinder: {
            float radial, axial;
            cylinderCoords(*this, p, radial, axial);
            return radial <= radius && std::abs(axial) <= halfLength && inBox(p, min, max);
        }
        case ShapeType::Heightfield:
            return inBox(p, min, max) && p.y <= field.heightAt(p.x, p.z);
    }
    return false;
}

bool Shape::containsEroded(const glm::vec3& p, float d) const {
    if (d <= 0.0f) return contains(p);
    const glm::vec3 lo = min + glm::vec3(d);
    const glm::vec3 hi = max - glm::vec3(d);
    switch (type) {
        case ShapeType::Box:
            return inBox(p, lo, hi);
        case ShapeType::Sphere:
            return glm::length(p - center) <= radius - d && inBox(p, lo, hi);
        case ShapeType::Cylinder: {
            float radial, axial;
            cylinderCoords(*this, p, radial, axial);
            return radial <= radius - d && std::abs(axial) <= halfLength - d && inBox(p, lo, hi);
        }
        case ShapeType::Heightfield:
            // Vertical erosion only. For a surface of slope m the true
            // normal-distance erosion would be d*sqrt(1+m^2); at the
            // slopes these scenarios use (<= 0.3) that is a <5% thicker
            // shell than requested, which errs toward more boundary
            // particles rather than fewer -- the safe direction, since a
            // shell that is too thin is exactly how fluid leaks.
            return inBox(p, lo, hi) && p.y <= field.heightAt(p.x, p.z) - d;
    }
    return false;
}

glm::vec3 Shape::boundsMin() const {
    switch (type) {
        case ShapeType::Sphere:
            return glm::max(min, center - glm::vec3(radius));
        case ShapeType::Cylinder: {
            glm::vec3 e(radius);
            e[axis == 0 ? 0 : (axis == 2 ? 2 : 1)] = halfLength;
            return glm::max(min, center - e);
        }
        default:
            return min;
    }
}

glm::vec3 Shape::boundsMax() const {
    switch (type) {
        case ShapeType::Sphere:
            return glm::min(max, center + glm::vec3(radius));
        case ShapeType::Cylinder: {
            glm::vec3 e(radius);
            e[axis == 0 ? 0 : (axis == 2 ? 2 : 1)] = halfLength;
            return glm::min(max, center + e);
        }
        default:
            return max;
    }
}

float Shape::approximateVolume() const {
    const glm::vec3 e = glm::max(boundsMax() - boundsMin(), glm::vec3(0.0f));
    switch (type) {
        case ShapeType::Sphere:
            return 4.0f / 3.0f * 3.14159265f * radius * radius * radius;
        case ShapeType::Cylinder:
            return 3.14159265f * radius * radius * 2.0f * halfLength;
        default:
            return e.x * e.y * e.z;
    }
}

namespace {

// Shared lattice walk. The loop bounds are computed from the bounding box
// in integer steps so the lattice is exactly reproducible: no accumulation
// of `x += spacing`, which would drift differently depending on where the
// box starts.
template <typename Accept>
void latticeWalk(const glm::vec3& lo, const glm::vec3& hi, float spacing, Accept&& accept,
                  std::vector<glm::vec3>& out) {
    if (spacing <= 0.0f) return;
    const glm::vec3 span = hi - lo;
    if (span.x < 0.0f || span.y < 0.0f || span.z < 0.0f) return;

    const int nx = static_cast<int>(std::floor(span.x / spacing)) + 1;
    const int ny = static_cast<int>(std::floor(span.y / spacing)) + 1;
    const int nz = static_cast<int>(std::floor(span.z / spacing)) + 1;

    for (int ix = 0; ix < nx; ++ix) {
        for (int iy = 0; iy < ny; ++iy) {
            for (int iz = 0; iz < nz; ++iz) {
                const glm::vec3 p = lo + glm::vec3(static_cast<float>(ix),
                                                    static_cast<float>(iy),
                                                    static_cast<float>(iz)) * spacing;
                if (accept(p)) out.push_back(p);
            }
        }
    }
}

} // namespace

void sampleVolume(const Shape& shape, float spacing, std::vector<glm::vec3>& out) {
    latticeWalk(shape.boundsMin(), shape.boundsMax(), spacing,
                 [&](const glm::vec3& p) { return shape.contains(p); }, out);
}

void sampleVolumeCentered(const Shape& shape, float spacing, std::vector<glm::vec3>& out) {
    if (spacing <= 0.0f) return;
    const glm::vec3 lo = shape.boundsMin();
    const glm::vec3 hi = shape.boundsMax();
    const glm::vec3 centre = 0.5f * (lo + hi);

    // Step out from the centre by whole spacings until the bounding box is
    // covered, so the centre is always sampled however thin the region is.
    glm::ivec3 steps;
    for (int a = 0; a < 3; ++a) {
        steps[a] = static_cast<int>(std::floor((hi[a] - lo[a]) * 0.5f / spacing));
    }
    const glm::vec3 anchor = centre - glm::vec3(steps) * spacing;
    const glm::vec3 far = centre + glm::vec3(steps) * spacing;
    latticeWalk(anchor, far + glm::vec3(spacing * 0.5f), spacing,
                 [&](const glm::vec3& p) { return shape.contains(p); }, out);
}

void sampleShell(const Shape& shape, float spacing, int layers, std::vector<glm::vec3>& out) {
    const float d = static_cast<float>(std::max(1, layers)) * spacing;
    latticeWalk(shape.boundsMin(), shape.boundsMax(), spacing,
                 [&](const glm::vec3& p) {
                     return shape.contains(p) && !shape.containsEroded(p, d);
                 }, out);
}

void sampleBoxWalls(const glm::vec3& lo, const glm::vec3& hi, float spacing, int layers,
                     const bool faceSolid[6], std::vector<glm::vec3>& out) {
    const int L = std::max(1, layers);
    const float pad = static_cast<float>(L) * spacing;

    // Walk the padded box and keep points that are OUTSIDE the fluid
    // domain, on a face that is solid. The lattice is anchored at
    // (lo - pad) so the wall layers sit at exact multiples of `spacing`
    // from the domain face -- a wall whose innermost layer is a fractional
    // spacing from the fluid is the classic cause of a persistent density
    // ripple along the floor.
    const glm::vec3 outerLo = lo - glm::vec3(pad);
    const glm::vec3 outerHi = hi + glm::vec3(pad);

    latticeWalk(outerLo, outerHi, spacing, [&](const glm::vec3& p) {
        const bool belowX = p.x < lo.x - 0.5f * spacing;
        const bool aboveX = p.x > hi.x + 0.5f * spacing;
        const bool belowY = p.y < lo.y - 0.5f * spacing;
        const bool aboveY = p.y > hi.y + 0.5f * spacing;
        const bool belowZ = p.z < lo.z - 0.5f * spacing;
        const bool aboveZ = p.z > hi.z + 0.5f * spacing;
        if (!(belowX || aboveX || belowY || aboveY || belowZ || aboveZ)) return false;

        // A corner point belongs to several faces at once; it is kept if
        // ANY of the faces it lies outside of is solid, so an open face
        // does not punch a hole through the adjacent solid walls' corners.
        if (belowX && faceSolid[0]) return true;
        if (aboveX && faceSolid[1]) return true;
        if (belowY && faceSolid[2]) return true;
        if (aboveY && faceSolid[3]) return true;
        if (belowZ && faceSolid[4]) return true;
        if (aboveZ && faceSolid[5]) return true;
        return false;
    }, out);
}

} // namespace aquasph
