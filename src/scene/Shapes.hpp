#pragma once
#include <vector>
#include <glm/glm.hpp>

namespace aquasph {

// One tagged-union shape instead of a Shape base class with five
// subclasses. Every consumer -- fluid regions, obstacles, emitters, sinks,
// metric probes -- needs exactly the same three questions answered
// (does it contain this point, does it contain this point after eroding
// the solid by d, what is its bounding box), all five answers are a
// handful of arithmetic, and the set is closed by the scenario schema.
// Virtual dispatch would buy nothing here and would put an allocation and
// an indirect call inside the sampling loops.
enum class ShapeType {
    Box,
    Sphere,
    Cylinder,
    // Terrain: solid everywhere BELOW a procedural surface y = f(x, z).
    // Not a mesh loader -- see the note on Heightfield below.
    Heightfield,
};

// Procedural terrain, evaluated analytically.
//
// WHY NOT A MESH. Loading arbitrary terrain meshes means a mesh format
// parser, a robust point-in-mesh test, and a watertightness story --
// a large surface area for a project whose terrain scenarios are Tier 2
// visualisations in the first place. An analytic height function gives
// slopes, beaches, channels, spillway crests and obstacle-strewn
// floodplains, is exactly reproducible, has no asset files to lose, and
// makes the erosion test (containsEroded) a one-line subtraction instead
// of a signed-distance-field build. The cost is that real survey terrain
// cannot be imported; that is stated in docs/scenarios.md rather than
// implied away, and mesh terrain is listed as a Tier 3 extension point.
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

    // True when p is inside the solid AND at least `d` away from its
    // surface (measured by the same analytic form, so it is exact for
    // boxes/spheres/cylinders and a good approximation for a heightfield
    // with a modest slope). This is how a *shell* of boundary particles is
    // produced: sample the volume, keep points that contains() accepts and
    // containsEroded() rejects.
    bool containsEroded(const glm::vec3& p, float d) const;

    glm::vec3 boundsMin() const;
    glm::vec3 boundsMax() const;

    // Volume of the shape, m^3, used to pre-size particle reservations.
    // Approximate for Heightfield (bounding-box based).
    float approximateVolume() const;
};

// Lattice points inside `shape`, at `spacing`, aligned to a grid anchored
// at the shape's bounding-box minimum. Anchoring the lattice to the shape
// (not to the domain) keeps a region's particle layout independent of
// where the domain happens to start, so two scenarios that differ only in
// domain size still produce identical fluid blocks.
void sampleVolume(const Shape& shape, float spacing, std::vector<glm::vec3>& out);

// As sampleVolume, but with the lattice anchored so that the shape's
// bounding-box CENTRE is itself a lattice point.
//
// This is the right sampling for an emitter aperture, and the difference
// is not cosmetic. An emitter is a cross-section: a round nozzle disc or
// a rectangular inlet, often thinner than one particle spacing. Anchored
// at the bounding-box minimum, such a region can contain no lattice point
// at all -- the single candidate plane lands a float ULP outside the
// shape's own half-thickness test -- and the emitter then silently emits
// nothing for the whole run. Anchoring at the centre guarantees the
// centre plane exists, and as a bonus makes a round aperture symmetric
// instead of biased toward one corner.
void sampleVolumeCentered(const Shape& shape, float spacing, std::vector<glm::vec3>& out);

// Lattice points inside `shape` but within `layers * spacing` of its
// surface -- the boundary-particle shell for a solid obstacle.
void sampleShell(const Shape& shape, float spacing, int layers, std::vector<glm::vec3>& out);

// The inside-out case: a shell OUTSIDE the box [min,max], used for tank
// walls. `faceSolid` selects which of the six faces (-x,+x,-y,+y,-z,+z)
// get boundary particles, so an open channel or a periodic flume simply
// omits them.
void sampleBoxWalls(const glm::vec3& min, const glm::vec3& max, float spacing, int layers,
                     const bool faceSolid[6], std::vector<glm::vec3>& out);

} // namespace aquasph
