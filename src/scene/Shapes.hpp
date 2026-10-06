#pragma once
#include <vector>
#include <glm/glm.hpp>

namespace aquasph {

// One tagged-union shape instead of a Shape base class with five subclasses.
enum class ShapeType {
    Box,
    Sphere,
    Cylinder,
    // Terrain: solid everywhere BELOW a procedural surface y = f(x, z).
    // Not a mesh loader -- see the note on Heightfield below.
    Heightfield,
};

// Procedural terrain, evaluated analytically. WHY NOT A MESH.
struct HeightfieldSpec {
    float base = 0.0f;                 // y at `origin`
    glm::vec2 origin{0.0f};            // (x, z) reference point
    glm::vec2 slope{0.0f};             // dy/dx, dy/dz
    // Gaussian bumps: amplitude, centre (x,z), sigma.
    struct Bump { float amplitude = 0.0f; glm::vec2 center{0.0f}; float sigma = 1.0f; };
    std::vector<Bump> bumps;
    // Clamps, so a beach slope does not run off to +-infinity outside the
    // region of interest.
    float minHeight = -1.0e9f;
    float maxHeight = 1.0e9f;

    float heightAt(float x, float z) const;
};

struct Shape {
    ShapeType type = ShapeType::Box;

    // Box: the box itself. Others: the region the shape is clipped to,
    // which also bounds sampling.
    glm::vec3 min{-1.0f};
    glm::vec3 max{1.0f};

    // Sphere / Cylinder.
    glm::vec3 center{0.0f};
    float radius = 0.5f;
    float halfLength = 0.5f;   // Cylinder, along `axis`
    int axis = 1;              // Cylinder axis: 0 = x, 1 = y, 2 = z

    HeightfieldSpec field;

    bool contains(const glm::vec3& p) const;

    // True when p is inside the solid AND at least `d` away from its surface (measured by the
    // same analytic form, so it is exact for boxes/spheres/cylinders and a good approximation
    // for a heightfield with a modest slope).
    bool containsEroded(const glm::vec3& p, float d) const;

    glm::vec3 boundsMin() const;
    glm::vec3 boundsMax() const;

    // Volume of the shape, m^3, used to pre-size particle reservations.
    // Approximate for Heightfield (bounding-box based).
    float approximateVolume() const;
};

// Lattice points inside `shape`, at `spacing`, aligned to a grid anchored at the shape's
// bounding-box minimum.
void sampleVolume(const Shape& shape, float spacing, std::vector<glm::vec3>& out);

// As sampleVolume, but with the lattice anchored so that the shape's bounding-box CENTRE is
// itself a lattice point.
void sampleVolumeCentered(const Shape& shape, float spacing, std::vector<glm::vec3>& out);

// Lattice points inside `shape` but within `layers * spacing` of its
// surface -- the boundary-particle shell for a solid obstacle.
void sampleShell(const Shape& shape, float spacing, int layers, std::vector<glm::vec3>& out);

// The inside-out case: a shell OUTSIDE the box [min,max], used for tank walls.
void sampleBoxWalls(const glm::vec3& min, const glm::vec3& max, float spacing, int layers,
                     const bool faceSolid[6], std::vector<glm::vec3>& out);

} // namespace aquasph
