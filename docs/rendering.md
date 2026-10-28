# Screen-space fluid rendering

The renderer follows van der Laan, Green & Sainz, *Screen Space Fluid
Rendering with Curvature Flow* (I3D 2009). **No mesh extraction and no
marching cubes**: the fluid surface is never built as geometry at all.
Particles are rasterised as sphere impostors into a depth buffer, that
depth image is smoothed, and normals are taken from the smoothed depth.

The cost is therefore per-pixel rather than per-cell-cubed, and the
surface follows the particles exactly, because it *is* the particles seen
from where the camera happens to be.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DAQUASPH_BUILD_VISUALIZATION=ON
cmake --build build --parallel

./build/aquasph_view --scenario dam_break --quality medium
./build/aquasph_view --scenario droplet_impact --render-mode points
./build/aquasph_view --scenario waterfall --quality high \
                      --size 1920 1080 --record-headless --record frames/waterfall
scripts/make_video.sh frames/waterfall 30
```

Controls: left-drag orbits, scroll zooms, **SPACE** pauses, **M** toggles
surface/points, **ESC** quits.

---

## The pipeline

Five passes. Everything except the first and fourth is a full-screen
triangle.

### 1. Depth — sphere impostors

Each particle is one `GL_POINT`, sized by the projected diameter of a
sphere at that distance (`gl_PointSize = viewportHeight / (2 tan(fov/2)) ·
r / |z_view|`). The fragment shader rebuilds the front of the sphere from
`gl_PointCoord`, discards fragments outside the disc, and writes the
**true sphere-surface depth** through `gl_FragDepth`.

Writing true depth rather than the sprite's flat centre depth is what
makes impostors intersect each other and the scene correctly instead of
behaving like cardboard cut-outs. Linear eye-space depth goes to an R32F
texture, cleared to a large sentinel — **not** to zero, because zero is a
perfectly valid eye depth and clearing to it makes the whole background
read as fluid pressed against the lens.

**Draw radius is 0.9 × the particle spacing.** At 0.62 the spheres overlap
along the lattice axes but not along its diagonals, where neighbours are
√3 times further apart; the depth image came out peppered with holes that
fell through to the background and rendered as black speckle across the
whole surface. 0.9 covers the 3D diagonal. Much larger and the fluid
visibly inflates past its own volume.

### 2. Depth smoothing — separable bilateral filter

Run `smoothIterations` times (2 / 4 / 6 for low / medium / high),
horizontally then vertically.

Bilateral rather than Gaussian, because the filter **must not smooth
across a depth discontinuity**. Two guards, and both are needed:

1. Background texels are excluded from the sum entirely. Averaging a
   silhouette against a depth of 1e6 would drag the fluid's edge a long
   way back and produce the bright fringe that gives screen-space fluid
   away.
2. Remaining samples are weighted by `exp(−((s − centre)·falloff)²)`, so
   two sheets of fluid that merely overlap on screen do not bleed into one
   another.

`depthFalloff` is in inverse metres and is set from the particle spacing,
not fixed: a falloff tuned for a 2 m tank would treat every depth step in
a 9 cm droplet scene as continuous and smooth the crown flat.

**A halo at the silhouette is a bug, not a look.** If you see one, this
pass is where it is.

### 3. Normals — finite differences of the smoothed depth

View-space position is reconstructed from the smoothed depth and the
inverse projection, then differenced. Never from particle geometry, which
would put the particles back into the surface the smoothing just removed.

Forward and backward differences are compared and the **smaller** taken:
at a silhouette one of the two straddles the edge and is enormous, so
taking the smaller keeps the normal on the surface instead of tipping it
toward whatever is behind.

A neighbour that is *background* is clamped to the centre pixel's depth
rather than sampled at the sentinel. Without that clamp, a silhouette
pixel differences against a position a kilometre away and gets a garbage
normal — which showed up as a ring of black speckle around the fluid and
along every thin sheet.

### 4. Thickness — additive accumulation

Particles again, this time with the **depth test off** and additive
blending, each contributing the chord length through its sphere,
`2r√(1−ρ²)`. Every particle along the ray must contribute, including the
ones behind the front surface: that is what makes this an optical path
length rather than a silhouette. Then a light 5×5 Gaussian — thickness has
no silhouettes to preserve, so the bilateral machinery would only cost
time.

### 5. Composite

```
transmittance = exp(−σ_a · d)                      per RGB channel
opticalDepth  = 1 − exp(−σ_s · d)                  SCALAR
body          = refracted · transmittance + tint · opticalDepth
F             = 0.02 + 0.98 (1 − cos θ)^5          Schlick, water/air
colour        = mix(body, environment(reflect), F) + specular
```

- **Refraction** offsets the background sample along the surface normal,
  by more where the fluid is thicker, clamped so a near-silhouette normal
  cannot fling the sample across the frame.
- **Beer–Lambert absorption** is where the colour comes from — not a
  diffuse albedo. Deep volume tints strongly; thin sheets stay nearly
  clear. That is why a splash sheet and a settled pool read as the same
  fluid at different depths rather than as two different materials.
- **The scattering term uses a scalar optical depth.** Using the
  per-channel `1 − transmittance` is the obvious thing to write and it is
  wrong: that quantity is largest in whichever channel is absorbed *most*,
  so the emergent colour comes out as the complement of the fluid's
  absorption spectrum. Water, absorbed most strongly in red, renders
  brown. The first working frames of the dam break came out the colour of
  rust.
- **Specular** is one lobe per light from the scenario's three-point rig,
  each modulated by Fresnel at its **own half-vector** — not by the
  view-normal `F` used for the reflection mix. Reusing `F` leaves a
  face-on surface with essentially no highlight, since F is 0.02 for water
  at normal incidence, and the fluid reads as flat coloured glass.
- **The environment is analytic**: a cool sky above, a darker ground
  below, horizon softened. Deliberately not a loaded HDR cubemap — this
  exists to give the specular and the reflection something plausible to
  pick up, and a texture asset would be one more thing to ship and to get
  wrong.

---

## Environment and art direction

One visual language across every scenario, so the project reads as a
single visualisation system rather than a dozen unrelated demos.

- **Lighting** is three-point — key, dimmer fill, rim behind the fluid —
  and fixed in **world** space, not attached to the camera, so the
  specular sweeps across the surface as the camera orbits.
- **Floor**: matte, with an analytically antialiased scale grid whose
  pitch is rounded to a readable quantity (1, 2 or 5 × a power of ten), so
  it conveys scale rather than being texture.
- **Contact shadow**: a top-down orthographic thickness pass, sampled by
  the floor shader in world XZ. Cheap, and physically the right shape —
  more fluid overhead means a darker patch of floor. It is what makes the
  fluid look like it is *on* the floor rather than floating in front of
  it.
- **Obstacles** are drawn as opaque matte solids from their **own boundary
  particles**. No mesh generation and no per-shape rendering code, and an
  obstacle that leaks in the simulation looks wrong here too, rather than
  being covered up by an idealised mesh.
- **Camera**: fixed three-quarter, identical framing convention across
  scenarios, optional slow constant orbit driven by *simulated* time so a
  recorded orbit is identical on a fast machine and a slow one.
- **Restraint**: no bloom, no lens flare, no depth of field, no chromatic
  aberration, no fake motion blur, no heavy grading. Each of those reads
  as compensation for a weak render.

### Points mode is not a legacy path

`--render-mode points` (or **M** in the viewer) draws speed-coloured
sprites. It is the diagnostic view: when a reconstructed surface looks
wrong, the first question is whether the particle distribution underneath
it is wrong, and only this mode answers that. It is also the performance
baseline the surface is measured against. Its colour ramp normalises
against the scenario's `render.reference_speed`, never against a physics
limit.

---

## Quality presets

Simulation resolution and visual quality are different problems, and
conflating them is why the presets exist.

| preset | resolution scale | smoothing iterations | intent |
|---|---|---|---|
| `low` | ×1.8 spacing | 2 | development and debugging |
| `medium` | ×1.0 | 4 | interactive exploration |
| `high` | ×0.55 | 6 | offline showcase rendering, no frame budget |

Particle counts follow the geometry and are printed by every run rather
than promised here; `benchmarks/scaling_results.md` has the measured
figures for the dam break. A screen-space surface reconstructed from a few
thousand particles reads as lumpy no matter how good the shader is — the
surface simply is not there to find. That is a resolution problem, and
raising the blur radius makes it worse, not better.

---

## Capture

```bash
--record DIR          # PNG frames at the scenario's output cadence
--record-headless     # render into an FBO with no visible window, then exit
```

Frames are emitted on **simulated** time, so a clip plays back at a
defined rate regardless of how long each step took to compute.

`--record-headless` creates a *hidden* window and renders into a
framebuffer object: no window ever appears and nothing is swapped to a
front buffer. It does **not** remove the need for a display connection —
GLFW cannot create an OpenGL context without one. On a machine with no X
server, run it under a virtual display:

```bash
xvfb-run -s "-screen 0 1600x900x24" ./build/aquasph_view \
    --scenario dam_break --quality high --record-headless --record frames/dam
```

That path is not theoretical: every frame in this repository's gallery was
produced exactly that way, on Mesa's `llvmpipe` software rasteriser, which
provides a GL 4.5 core context with no GPU present.

`scripts/make_video.sh <dir> [fps]` assembles a frame sequence into MP4
and a looping GIF. `scripts/make_gallery.sh [quality]` does every scenario
plus a contact sheet.

**ffmpeg is an external dependency**, on purpose: linking a multimedia
framework to turn a numbered image sequence into a video would be far
larger than the task justifies.
`sudo apt install ffmpeg` / `brew install ffmpeg`.

---

## Platform requirements

The headless solver and the entire test suite link neither GL nor GLFW, so
`AQUASPH_BUILD_VISUALIZATION` defaults to **OFF** and a machine without GL
development headers still builds and tests everything else.

**Linux/X11.** GLFW's X11 backend needs development *headers* at configure
time, not merely the runtime shared libraries:

```bash
sudo apt install libglfw3-dev mesa-common-dev libgl1-mesa-dev \
  libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libx11-dev
# and, for headless capture:
sudo apt install xvfb
```

A system carrying the runtime `.so` files but not the `-dev` packages
fails GLFW's own configure step with `RandR headers not found`, which is
the most common cause of a failed visualization build on Linux.

**macOS.** No extra system packages beyond a standard Xcode toolchain, but
the OpenMP cache variables from the README are still required, since
`aquasph_view` links `aquasph_core`.

**A note on the hand-written GL loader.** Modern OpenGL entry points are
not guaranteed to be link-time symbols; the portable way to obtain them is
a runtime lookup, which GLFW exposes as `glfwGetProcAddress`. A loader is
therefore the correct tool rather than a workaround, and `GLLoader.hpp`
declares exactly the ~60 entry points this renderer calls instead of
pulling in a multi-thousand-line generated header. Every signature and
enum value was cross-checked against the Khronos registry; see
`docs/architecture.md` for an error that check caught.

---

## Known rendering limitations

- **`gl_PointSize` has an implementation-defined ceiling** (commonly 63 or
  255). Sprites are clamped to it, so a camera very close to the fluid can
  under-size the impostors and open holes in the surface. Pull the camera
  back or raise the resolution rather than raising the draw radius.
- **No refraction of the fluid through itself.** The composite samples the
  *scene* colour behind the fluid; a second body of fluid behind the first
  is not refracted, only occluded.
- **No caustics, no foam, no spray, no air entrainment.** A real plunge
  pool turns white; this one does not, because the solver is single-phase.
  Listed as Tier 3 extension points, not implemented for appearance.
- **The contact shadow is an approximation.** It is a top-down thickness
  map, not a shadow from the key light's direction, so it does not shift
  as the lighting changes.
- **Thickness is scaled per scenario**, because absorption coefficients
  are per metre and these scenes are decimetres across. The tuning that
  makes a 0.3 m pool read as tinted water is a visual choice, stated here
  rather than presented as physics.
