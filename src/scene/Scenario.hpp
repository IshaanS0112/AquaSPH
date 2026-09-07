#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "Shapes.hpp"
#include "TimeSeries.hpp"
#include "../core/Material.hpp"
#include "../core/Integrator.hpp"
#include "../core/TimeStep.hpp"

namespace aquasph {

// A scenario is a COMPOSITION of primitives, never a subclass.
//
// The test this architecture has to pass: adding a new phenomenon must
// mean new JSON plus existing primitives, not new solver code. If a
// scenario cannot be expressed here without touching ForceCompute or
// Integrator, the primitive set is wrong and the fix belongs in the
// primitive set. docs/scenarios.md records which scenarios in the shipped
// library needed which primitives, so that claim is checkable rather than
// asserted.

// --- Credibility tier -------------------------------------------------
// Carried in the scenario itself, printed in every run's output, and
// emitted into the metrics JSON, so a result cannot circulate detached
// from what it is claiming. See README.md and docs/gallery.md.
enum class Tier {
    // Physically demonstrable: scales and phenomena weakly-compressible
    // SPH genuinely resolves at achievable particle counts, checkable
    // against experiment or an analytical result.
    One = 1,
    // Large-scale visual experiment: qualitatively informative, not
    // quantitatively predictive at these resolutions. Honest as a
    // demonstration, dishonest as a forecast.
    Two = 2,
};

const char* tierLabel(Tier t);
const char* tierCaveat(Tier t);

// --- Domain -----------------------------------------------------------
struct Domain {
    glm::vec3 min{-1.0f};
    glm::vec3 max{1.0f};
    // -x, +x, -y, +y, -z, +z
    FaceMode faces[6] = {FaceMode::Solid, FaceMode::Solid, FaceMode::Solid,
                          FaceMode::Solid, FaceMode::Solid, FaceMode::Solid};
    // Generate Akinci boundary particles along the Solid faces. Off gives
    // the old clamp-and-damp box, which is kept only so the v1 dam break
    // can be reproduced for the before/after comparison.
    bool boundaryParticles = true;
    int boundaryLayers = 2;

    // Boundary particle spacing, as a fraction of the fluid spacing.
    //
    // WHY THIS IS A KNOB AND NOT FIXED AT 1.0. The Akinci volume weighting
    // makes the wall's strength independent of its sampling density only
    // once enough boundary layers fall inside the kernel support. The
    // number that do is roughly h/spacing - 1, so at this project's
    // h/spacing = 2 a fluid particle resting on a wall sees exactly ONE
    // layer. Measured directly (see docs/architecture.md, "How well the
    // boundary is resolved"): at h/spacing = 2 the boundary sum recovers
    // only ~28% of the density a solid half-space should contribute,
    // rising to ~83% at 4 and ~112% at 8. Halving the boundary spacing
    // costs 8x the boundary particles -- which do no work in the fluid
    // loops, but do enlarge near-wall neighbour lists -- so it is a
    // measured trade rather than a default.
    float boundarySpacingScale = 1.0f;
    // Velocity retained on a containment hit. With boundary particles on,
    // containment should essentially never fire; Simulation counts it.
    float containmentDamping = 0.0f;
};

// --- Initial fluid volumes -------------------------------------------
struct FluidRegion {
    Shape shape;
    std::string material = "water";

    enum class VelocityProfile {
        Uniform,      // v = velocity
        Shear,        // v = velocity + shearRate * (x[shearAxis] - shearOrigin) along shearDir
        Vortex,       // solid-body rotation about `shearAxis` through the region centre
    };
    VelocityProfile profile = VelocityProfile::Uniform;
    glm::vec3 velocity{0.0f};
    glm::vec3 shearDir{1.0f, 0.0f, 0.0f};
    int shearAxis = 1;
    float shearOrigin = 0.0f;
    float shearRate = 0.0f;   // 1/s (Shear) or rad/s (Vortex)

    glm::vec3 velocityAt(const glm::vec3& p, const glm::vec3& regionCentre) const;
};

// --- Emitters ---------------------------------------------------------
struct Emitter {
    // Emission cross-section. Particles are created on a lattice covering
    // this shape and pushed out along `direction`.
    Shape shape;
    std::string material = "water";
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    float speed = 1.0f;              // m/s along `direction`
    TimeSeries schedule;             // dimensionless multiplier on `speed`
    float startTime = 0.0f;
    float endTime = -1.0f;           // < 0 => runs to the end of the scenario

    // EMISSION MODEL. One full lattice layer is released whenever the
    // stream has advanced a full `spacing` since the last release
    // (accumulated as speed(t) * dt). Volumetric flow is then
    // area * speed by construction, and a schedule that halves the speed
    // halves the flow rate -- which is what makes a flash-flood hydrograph
    // expressible as a TimeSeries rather than as a special emitter type.
    //
    // Consequence, stated because it drives the whole dynamic-array audit
    // in docs/architecture.md: the particle array GROWS during the run.
    float layerDebt = 0.0f;          // runtime state, metres of un-emitted stream
};

// --- Sinks ------------------------------------------------------------
struct Sink {
    // Fluid particles whose centre falls inside are removed. Drains,
    // outlets, and absorbing beaches. Without sinks any open-domain
    // scenario simply fills up and stops meaning anything.
    Shape shape;
    float startTime = 0.0f;
    float endTime = -1.0f;
};

// --- Obstacles and moving boundaries ----------------------------------
struct Motion {
    // WHAT IS IMPLEMENTED: prescribed TRANSLATION along a fixed axis,
    // driven by a TimeSeries. That is enough for wave paddles, pistons,
    // sliding gates and shaken tanks, and it is what the wave generators
    // below are built on.
    //
    // WHAT THE INTERFACE PERMITS BUT DOES NOT DO: rotation. An
    // orientationAt(t) would slot in beside translationAt(t) and would
    // need the boundary particles re-transformed rather than merely
    // offset; the Akinci boundary volumes survive both, since a rigid
    // motion does not change inter-boundary distances.
    //
    // WHAT IS OUT OF SCOPE ENTIRELY: rigid-body DYNAMICS -- the fluid
    // exerting force back on a free solid. Nothing here integrates a
    // solid's momentum. A floating body is a Tier 3 extension point, not
    // a switch away.
    glm::vec3 axis{1.0f, 0.0f, 0.0f};
    // Explicitly zero, not TimeSeries's own default: a default-constructed
    // TimeSeries is the constant 1, which would silently make every
    // obstacle a metre out of place and non-static.
    TimeSeries displacement = TimeSeries::zero();

    bool isStatic() const;
    glm::vec3 translationAt(float t) const;
    // Central difference of translationAt. Uniform across every TimeSeries
    // kind including Keyframes, and exactly reproducible, which matters
    // because this velocity enters the boundary friction term.
    glm::vec3 velocityAt(float t) const;
};

struct Obstacle {
    std::string name = "obstacle";
    Shape shape;
    Motion motion;
};

// --- External forces --------------------------------------------------
struct ExternalForce {
    std::string name = "force";
    glm::vec3 direction{1.0f, 0.0f, 0.0f};
    TimeSeries magnitude;   // m/s^2 along `direction`
};

// --- Wave generation --------------------------------------------------
struct WaveComponent {
    float amplitude = 0.05f;   // paddle stroke half-amplitude, m
    float period = 1.0f;       // s
    float phase = 0.0f;        // rad
};

struct WaveGenerator {
    // A PISTON WAVEMAKER: a slab of boundary particles at one end of the
    // tank, translating horizontally on a prescribed schedule. This is a
    // real, standard laboratory wave-making mechanism, which is exactly
    // why it was chosen over imposing a free-surface displacement
    // eta(x,t) directly: a prescribed surface is a rendering trick that
    // the fluid does not actually obey, whereas a paddle makes waves the
    // solver has to propagate itself, so amplitude, period and celerity
    // become measurements rather than inputs.
    //
    // The linear (Biesel) transfer function for a piston in water of
    // depth d relates the paddle stroke S = 2a to wave height H:
    //     H/S = 2 (cosh(2kd) - 1) / (sinh(2kd) + 2kd)
    // with k from the dispersion relation omega^2 = g k tanh(k d). The
    // predicted H, wavelength and celerity are emitted into the metrics
    // alongside the measured values -- see docs/validation.md. Linear
    // theory holds for small steepness (H/L well under ~1/20); beyond
    // that the comparison is reported but flagged, not quietly used.
    std::string name = "paddle";
    int axis = 0;                 // travel direction: 0 = x, 1 = y, 2 = z
    float position = -1.0f;       // paddle rest plane along `axis`
    float thickness = 0.06f;      // paddle slab thickness, m
    glm::vec3 spanMin{-1.0f};     // paddle extent in the other two axes
    glm::vec3 spanMax{1.0f};

    enum class Mode { Sinusoidal, Pulse, Damped, Superposition };
    Mode mode = Mode::Sinusoidal;

    float amplitude = 0.05f;
    float period = 1.0f;
    float phase = 0.0f;
    float rampTime = 0.0f;
    float decay = 0.0f;
    float startTime = 0.0f;
    float duration = -1.0f;       // < 0 => whole run
    std::vector<WaveComponent> components;   // Superposition only

    // Still-water depth used for the linear-theory prediction. Set from
    // the scenario's initial fluid depth; 0 disables the prediction.
    float stillWaterDepth = 0.0f;

    Obstacle toObstacle() const;
};

// --- Numerics ---------------------------------------------------------
struct Numerics {
    float h = 0.1f;                 // smoothing radius at quality "medium"
    float spacingRatio = 0.5f;      // particle spacing = spacingRatio * h
    TimeStepParams timestep;
    float xsphEpsilon = 0.5f;
    float boundaryFriction = 1.0f;
    // Multiplies h and spacing together, so the h/spacing ratio -- and
    // therefore the neighbour count each particle sees -- is invariant
    // across quality presets. Set by --quality, not by the scenario file.
    float resolutionScale = 1.0f;

    float effectiveH() const { return h * resolutionScale; }
    float spacing() const { return effectiveH() * spacingRatio; }
};

struct Duration {
    float simulatedTime = 2.0f;     // s
    float outputInterval = 0.02f;   // s between metric samples / captured frames
    int maxSteps = 2000000;         // hard stop, so a pathological dt cannot hang CI
};

// --- Presentation -----------------------------------------------------
struct CameraSpec {
    glm::vec3 target{0.0f};
    float distance = 4.0f;
    float yawDeg = 45.0f;
    float pitchDeg = 22.0f;
    float fovDeg = 40.0f;
    // Degrees per second of constant orbit. 0 = fixed camera. Never
    // anything faster or jittery: identical framing across scenarios is
    // what lets a contact sheet compare physics rather than composition.
    float orbitRateDegPerSec = 0.0f;
};

struct LightingSpec {
    // Three-point, fixed in WORLD space rather than attached to the
    // camera, so the specular sweeps across the surface as the camera
    // orbits instead of staying nailed to it.
    glm::vec3 keyDirection{-0.45f, -0.80f, -0.40f};
    glm::vec3 keyColor{1.0f, 0.97f, 0.92f};
    float keyIntensity = 1.0f;
    glm::vec3 fillDirection{0.65f, -0.25f, 0.55f};
    glm::vec3 fillColor{0.62f, 0.74f, 0.85f};
    float fillIntensity = 0.32f;
    glm::vec3 rimDirection{0.15f, 0.35f, 0.92f};
    glm::vec3 rimColor{0.80f, 0.92f, 1.0f};
    float rimIntensity = 0.55f;
};

struct RenderSettings {
    enum class Mode { Points, Surface };
    Mode mode = Mode::Surface;

    // Speed (m/s) that maps to the top of the colour ramp. PER SCENARIO
    // and never the integrator's clamp: v1 normalised by max_speed while
    // the integrator clamped to exactly max_speed, so during the
    // interesting phase nearly every particle mapped to 1.0 and rendered
    // white. Visualisation normalisation must never affect physics, and
    // physics limits must never set visualisation scale.
    float referenceSpeed = 4.0f;

    std::string palette = "teal";
    float pointSizePixels = 6.0f;
    bool showGrid = true;
    bool showDomainWireframe = true;
    bool showFloor = true;
};

// --- Measurement ------------------------------------------------------
struct WaveProbe {
    std::string name = "probe";
    glm::vec3 position{0.0f};   // x,z of the vertical water column to measure
    float radius = 0.08f;
};

struct MetricsSpec {
    // Surge-front tracking along an axis, for dam-break style scenarios.
    bool trackSurgeFront = false;
    int surgeAxis = 0;
    float surgeOrigin = 0.0f;       // x0 of the initial column face
    float surgeColumnWidth = 0.0f;  // a, the Martin & Moyce length scale
    float surgeColumnHeight = 0.0f; // the initial column height

    // Water-depth probes for waves, floods, container fill.
    std::vector<WaveProbe> probes;

    // Inundation reporting: fraction of the floor covered above this depth.
    bool trackInundation = false;
    float inundationDepth = 0.02f;
};

// --- The scenario -----------------------------------------------------
struct Scenario {
    std::string name = "unnamed";
    std::string description;
    Tier tier = Tier::One;
    // One honest sentence about where this scenario's model is
    // approximate. Printed with the results and reproduced in the gallery.
    std::string approximation;

    Domain domain;
    MaterialTable materials{Material{}};
    std::vector<FluidRegion> fluidRegions;
    std::vector<Emitter> emitters;
    std::vector<Sink> sinks;
    std::vector<Obstacle> obstacles;
    std::vector<ExternalForce> externalForces;
    std::vector<WaveGenerator> waveGenerators;

    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    Numerics numerics;
    Duration duration;
    CameraSpec camera;
    LightingSpec lighting;
    RenderSettings render;
    MetricsSpec metrics;

    // Index into `materials`, or 0 with a warning if the name is unknown.
    std::uint8_t materialIndex(const std::string& name) const;
};

} // namespace aquasph
