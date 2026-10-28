# Authoring a scenario

A scenario is a **composition of primitives described in JSON**. The
fourteen scenarios in `configs/scenarios/` contain no C++ between them,
and adding a fifteenth should not require any either. If you find yourself
needing to edit `ForceCompute` or `Integrator` to express a phenomenon,
that is a missing primitive, not a missing special case — say so and add
the primitive.

```bash
./aquasph --list-scenarios
./aquasph --scenario dam_break --quality medium --metrics out.json
./aquasph --scenario ./my_scenario.json          # an explicit path also works
```

`--scenario NAME` searches `configs/scenarios/`, `../configs/scenarios/`
and `../../configs/scenarios/`, so it works from the repository root and
from inside `build/`.

---

## The shape of a scenario file

Every key is optional and falls back to a documented default. Unknown keys
are ignored, so a file written for a later version still loads. A
malformed *value* — a shape with an unrecognised type, a material name
that is not defined — prints what it substituted rather than failing
silently or throwing.

```jsonc
{
  "name": "my_scenario",
  "tier": 1,                       // 1 or 2; see "Credibility tiers" below
  "description": "One line, shown by --list-scenarios.",
  "approximation": "One honest sentence about where this model is wrong.",

  "domain":       { ... },
  "gravity":      [0.0, -9.81, 0.0],
  "materials":    [ ... ],
  "fluid_regions":   [ ... ],
  "emitters":        [ ... ],
  "sinks":           [ ... ],
  "obstacles":       [ ... ],
  "external_forces": [ ... ],
  "wave_generators": [ ... ],
  "numerics":     { ... },
  "duration":     { ... },
  "camera":       { ... },
  "lighting":     { ... },
  "render":       { ... },
  "metrics":      { ... }
}
```

---

## Credibility tiers

`tier` is not decoration. It is printed by every run, included in every
metrics file, and reproduced in `docs/gallery.md`, so a result cannot
circulate detached from what it claims.

- **Tier 1 — physically demonstrable.** Scales and phenomena that
  weakly-compressible SPH genuinely resolves at achievable particle
  counts, and that can be checked against experiment or an analytical
  result.
- **Tier 2 — large-scale visual experiment.** Qualitatively informative,
  visually compelling, and *not quantitatively predictive at these
  resolutions*. Honest as a demonstration, dishonest as a forecast.

`approximation` is where a Tier 2 scenario states plainly what it is not.
Look at `flood.json` or `tsunami_pulse.json` for the tone: a flood
scenario says it is not a hydrological prediction, and the tsunami pulse
says in as many words that it is a long-wave propagation experiment and
not a tsunami model.

---

## Domain

```jsonc
"domain": {
  "min": [0.0, 0.0, 0.0],
  "max": [1.6, 0.6, 0.3],
  "faces": {                       // solid | open, per face
    "x_min": "solid", "x_max": "open",
    "y_min": "solid", "y_max": "open",
    "z_min": "solid", "z_max": "solid"
  },
  "boundary_particles": true,      // Akinci boundary particles on solid faces
  "boundary_layers": 2,
  "boundary_spacing_scale": 1.0,   // see the note below before changing this
  "containment_damping": 0.0
}
```

- **`solid`** faces get boundary particles and are a real wall.
- **`open`** faces are exits: particles that cross are removed, and are
  *not* counted as instability. Without at least one open face or a sink,
  any scenario with an emitter fills up and stops meaning anything.
**There is no `periodic` mode**, and its absence is deliberate rather
than an omission. Wrapping a particle's position at a face is four lines
and was implemented first; it does not produce a periodic domain. The
neighbour search computes separations directly, so a particle near one end
of the axis has no neighbours at the other — it sees a free surface
exactly where the domain is supposed to be continuous. Measured on a
driven channel, more than half the fluid was ejected within half a second.
A real periodic domain needs the minimum-image convention threaded through
the neighbour search and every force loop; it is listed as a Tier 3
extension point in `docs/architecture.md`, and asking for `"periodic"`
prints that explanation rather than silently substituting a wall.

Setting `y_max` to `open` on a scenario where nothing reaches the ceiling
is worth doing: it removes a whole face of boundary particles, which are
often the majority of the particle count in a shallow tank.

**`boundary_spacing_scale` is measured, not a free parameter.** Leave it
at 1.0 unless you have a reason and a measurement. See
`docs/architecture.md`, "How well the boundary is resolved" — sampling the
wall more finely over-pressurises the near-wall layer and ejects fluid.

---

## Materials

```jsonc
"materials": [
  { "name": "water",
    "rest_density": 1000.0,
    "sound_speed": 45.0,           // ARTIFICIAL; see below
    "gamma": 7.0,
    "viscosity": 5.0,              // Pa*s, artificial; see below
    "surface_tension": 0.0,        // Akinci (2013) coefficient
    "absorption": [0.55, 0.16, 0.09] }   // render-only, 1/m per RGB channel
]
```

Each fluid region and emitter names a material. Multiple materials can
coexist and each particle carries its own index — but **this is not
multiphase physics**. The solver is single-phase throughout: no interface
tension between materials, no density-ratio-stable pressure formulation,
no mixing model. A two-fluid setup will run; at a large density ratio the
interface will be dominated by numerical artefacts.

**Choosing `sound_speed`.** Weakly-compressible SPH wants
`c0 >= 10 * v_max` so density fluctuations stay near 1%, and no larger,
because the CFL step is `~0.25 h / c0`. Estimate `v_max` from the
scenario: a column of height `h0` collapsing gives roughly `2 sqrt(g h0)`.
Doubling `c0` doubles the run time for no physical gain.

**`viscosity` is artificial and much larger than water's 1e-3 Pa·s.** At
h = 0.02–0.1 m the simulation cannot resolve the turbulent cascade that
dissipates energy in a real flow; without a numerically enlarged
viscosity, that energy reappears as particle-scale noise. Treat it as a
sub-particle dissipation model and do not read Reynolds numbers off it.

**`surface_tension` is per material and per scenario**, and should usually
be 0. A metre-scale dam break has an enormous Weber number, so surface
tension is physically irrelevant there and only costs time. A droplet
impact needs it and cannot form a crown without it.

---

## Fluid regions — the initial volume

```jsonc
"fluid_regions": [
  { "type": "column", "material": "water",
    "base": [0.0, 0.0, 0.0], "size": [0.25, 0.5, 0.3] },

  { "type": "sphere", "material": "water",
    "center": [0.0, 0.045, 0.0], "radius": 0.012,
    "min": [-0.045, 0.0, -0.045], "max": [0.045, 0.09, 0.045],
    "velocity": [0.0, -2.0, 0.0] }
]
```

Shapes: `box` (alias `column`), `sphere`, `cylinder`, `heightfield`
(alias `terrain`). `min`/`max` always act as a clipping box, which is what
lets a sphere be cut off by the domain or a terrain be limited to part of
the floor. `base` + `size` is a friendlier spelling of `min` + `max`.

Particle mass is derived, never specified: `m = rho0 * spacing^3`, so a
uniform lattice sums to rest density. (Reading a mass from config instead
is the very first runtime bug this project hit — see
`docs/architecture.md`.)

Initial velocity fields:

```jsonc
"profile": "uniform"                                    // v = velocity
"profile": "shear",  "shear_axis": "y", "shear_rate": 2.0,
                      "shear_dir": [1,0,0], "shear_origin": 0.0
"profile": "vortex", "shear_axis": "y", "shear_rate": 3.0  // solid-body rotation
```

Arbitrary scripted velocity fields are **not** implemented: that needs an
expression parser, which is a dependency this project has not taken. The
three profiles above plus an emitter cover every scenario shipped here.

---

## Emitters — inflow

```jsonc
"emitters": [
  { "type": "cylinder", "material": "water",
    "center": [0.2, 0.4, 0.15], "radius": 0.035, "half_length": 0.006, "axis": "y",
    "min": [0,0,0], "max": [0.4, 0.45, 0.3],
    "direction": [0.0, -1.0, 0.0],
    "speed": 1.0,
    "schedule": { "kind": "ramp", "value": 1.0, "start": 0.0, "end": 0.3 },
    "start_time": 0.0,
    "end_time": 2.4 }
]
```

An emitter is an **aperture**. Its shape is sampled on a lattice once, and
one full layer is released whenever the stream has advanced a full
particle spacing (accumulated as `speed(t) * dt`). Volumetric flow is
therefore `area * speed` by construction, and `schedule` — a dimensionless
multiplier on `speed` — is how a hydrograph is expressed without a new
emitter type. See `flash_flood.json` for a keyframed one.

Emitters make the particle array **grow**. `--max-particles` (default 4
million, counting boundary particles) caps it; when the ceiling is
reached, emission stops with a warning rather than exhausting memory.

**An aperture thinner than one particle spacing will still work** — the
lattice is centre-anchored precisely so a thin disc always contains its
centre plane — but if it somehow produces no sites at all, the run says so
loudly on stderr. It used to fail silently, which is much worse.

---

## Sinks — outflow

```jsonc
"sinks": [ { "type": "box", "min": [2.3, 0, 0], "max": [2.4, 0.6, 1.2] } ]
```

Fluid whose centre falls inside is removed. `start_time` / `end_time`
optional. Use a sink for a drain in the middle of a domain; use an `open`
face for an outlet at its edge. Either is fine; an open face is cheaper.

---

## Obstacles and moving boundaries

```jsonc
"obstacles": [
  { "name": "cylinder", "type": "cylinder",
    "center": [0.6, 0.15, 0.2], "radius": 0.06, "half_length": 0.16, "axis": "y",
    "min": [0,0,0], "max": [1.2, 0.35, 0.4] },

  { "name": "gate", "type": "box", "min": [1,0,0], "max": [1.1,0.5,1],
    "motion": { "axis": [0, 1, 0],
                 "displacement": { "kind": "keyframes",
                                    "keys": [[0.0, 0.0], [1.0, 0.5]] } } }
]
```

An obstacle is sampled into a shell of boundary particles and handled by
the same Akinci coupling as the tank walls — so a new obstacle shape costs
the solver nothing.

`motion` prescribes **translation** along an axis, driven by a
`TimeSeries`. That covers wave paddles, pistons, sliding gates and shaken
tanks.

What is *not* implemented, stated plainly:

- **Rotation.** The interface would take an `orientationAt(t)` beside
  `translationAt(t)`; the boundary particles would need re-transforming
  rather than merely offsetting. The Akinci volumes survive either, since
  a rigid motion does not change inter-boundary distances.
- **Rigid-body dynamics.** Nothing integrates a solid's momentum, and the
  fluid exerts no force back on it. A floating body is a Tier 3 extension
  point, not a switch away.

### Terrain

```jsonc
{ "name": "beach", "type": "heightfield",
  "min": [1.6, 0, 0], "max": [3.0, 0.45, 0.3],
  "field": { "base": 0.0, "origin": [1.6, 0.0], "slope": [0.22, 0.0],
              "min_height": 0.0, "max_height": 0.32,
              "bumps": [ { "amplitude": 0.10, "center": [1.1, 0.35], "sigma": 0.16 } ] } }
```

Solid everywhere *below* `y = base + slope·(x−origin) + Σ Gaussian bumps`,
clamped to `[min_height, max_height]`. **This is not a mesh loader.**
Analytic terrain gives slopes, beaches, channels, spillway crests and
obstacle-strewn floodplains, is exactly reproducible, and has no asset
files to lose — but real survey terrain cannot be imported. Mesh terrain
is a documented Tier 3 extension point.

---

## Time-varying forcing

```jsonc
"external_forces": [
  { "name": "impulse", "direction": [1, 0, 0],
    "magnitude": { "kind": "pulse", "value": 2.0, "start": 0.1, "duration": 0.25 } }
]
```

Each force contributes `direction * magnitude(t)` to a uniform body
acceleration, in m/s². Gravity is just the first term; the solver does not
know which is which.

### `TimeSeries` — one f(t) used everywhere

Emitter schedules, external forces and prescribed obstacle motion all take
the same type. A bare number is shorthand for a constant.

| `kind` | fields | f(t) |
|---|---|---|
| `constant` | `value` | `value` |
| `ramp` | `value`, `start`, `end` | 0 → `value` linearly over `[start,end]`, then held |
| `pulse` | `value`, `start`, `duration` | `value` inside the window, else 0 |
| `sinusoidal` | `value`, `offset`, `period`, `phase`, `start`, `ramp_time` | `offset + value·sin(ωt+φ)`, smoothstep-enveloped over `ramp_time` |
| `damped` | as sinusoidal plus `decay` | sinusoidal × `exp(−decay·(t−start))` |
| `keyframes` (alias `scripted`) | `keys: [[t,v], ...]` | piecewise linear, held at both ends |

`ramp_time` matters more than it looks: starting a paddle impulsively
radiates a spurious transient down the flume that contaminates the whole
measurement.

---

## Wave generators

```jsonc
"wave_generators": [
  { "name": "piston", "axis": "x",
    "position": 0.1, "thickness": 0.05,
    "span_min": [0,0,0], "span_max": [2.4, 0.4, 0.25],
    "mode": "sinusoidal",          // sinusoidal | pulse | damped | superposition
    "amplitude": 0.02, "period": 0.9, "phase": 0.0,
    "ramp_time": 0.9, "decay": 0.0,
    "start_time": 0.0, "duration": -1,
    "components": [ { "amplitude": 0.01, "period": 0.7, "phase": 0.0 } ],
    "still_water_depth": 0.2 } ]
```

A wave generator **is** an obstacle with prescribed motion: a piston
paddle. That was chosen over imposing a free-surface displacement
`η(x,t)` directly, because a prescribed surface is a rendering trick the
fluid does not actually obey, whereas a paddle makes waves the solver has
to propagate itself. Amplitude, period and celerity therefore become
*measurements* rather than inputs.

`still_water_depth` enables the linear (Biesel) piston-wavemaker
prediction, emitted into the metrics beside the measurement:

```
H/S = 2 (cosh 2kd − 1) / (sinh 2kd + 2kd),   ω² = g k tanh(k d),   S = 2·amplitude
```

The prediction is reported only for `sinusoidal` and `damped` modes —
linear monochromatic theory does not describe a single pulse or a
superposition, and no number is better than a wrong one. It is also
flagged when the measured steepness exceeds H/L ≈ 1/20, beyond which it is
reported but should not be treated as a target.

**Do not raise the amplitude past what the resolution supports** to get a
more dramatic wave. Document the approximation and its validity range
instead.

---

## Numerics and duration

```jsonc
"numerics": {
  "h": 0.025,                // smoothing radius at quality "medium"
  "spacing_ratio": 0.5,      // particle spacing = spacing_ratio * h
  "xsph_epsilon": 0.5,       // Monaghan XSPH; 0 disables
  "boundary_friction": 1.0,  // 0 = free slip, 1 = no slip
  "cfl_coeff": 0.25, "force_coeff": 0.25, "viscous_coeff": 0.125,
  "dt_min": 1e-6, "dt_max": 0.001
},
"duration": { "simulated_time": 1.2, "output_interval": 0.01, "max_steps": 2000000 }
```

`--quality low|medium|high` multiplies **both** `h` and the spacing by
1.8 / 1.0 / 0.55, so the ratio between them — and therefore the number of
neighbours each particle sees — is identical across presets. Changing
quality changes how finely the fluid is sampled, never how
well-conditioned the discretisation is.

Particle count scales roughly as the inverse cube of that factor: `low` is
about 5.8× fewer than `medium`, `high` about 6× more. The actual counts
depend on your geometry and are printed by every run.

`output_interval` sets both the metric-sampling cadence and the recorded
frame cadence, in *simulated* seconds — so a clip plays back at a defined
rate regardless of how long each step took to compute.

---

## Measurement

```jsonc
"metrics": {
  "track_surge_front": true, "surge_axis": "x",
  "surge_origin": 0.0, "surge_column_width": 0.25, "surge_column_height": 0.5,
  "track_inundation": true, "inundation_depth": 0.03,
  "probes": [ { "name": "wg1", "position": [0.8, 0.0, 0.125], "radius": 0.03 } ]
}
```

- **Surge front** is the toe: the furthest-travelled particle still within
  three spacings of the floor. Taking the furthest particle overall would
  track spray, which is noisier and is not what Martin & Moyce measured.
  Reported as `Z = (front − surge_origin)/a` and `T = t·sqrt(2g/a)`.
- **Probes** record the free-surface elevation in a vertical column of
  radius `radius` at `(x, z)`. An empty column reads as dry rather than as
  a gap, so the record stays uniformly sampled and can be zero-crossed.
  Amplitude, period, wavelength and celerity are fitted from zero
  up-crossings at the end of the run.
- **Inundation** reports the fraction of floor cells whose water depth
  exceeds `inundation_depth`, on a grid tied to the particle spacing — so
  it is comparable across quality presets only to within one cell.

`--metrics out.json` writes all of it, plus particle counts, dt
statistics, density envelope, the fraction of particles within 1% of rest
density, fluid volume, containment events, and the git revision that
produced it.

---

## Presentation

```jsonc
"camera":   { "target": [0.8, 0.2, 0.15], "distance": 2.6,
              "yaw_deg": 38.0, "pitch_deg": 20.0, "fov_deg": 40.0,
              "orbit_rate_deg_per_sec": 0.0 },
"render":   { "mode": "surface", "reference_speed": 3.5, "palette": "teal",
              "point_size_pixels": 6.0,
              "show_grid": true, "show_domain_wireframe": true, "show_floor": true }
```

Keep `yaw_deg` at 38 and `pitch_deg` near 20 unless a scenario genuinely
needs otherwise: identical framing across scenarios is what lets the
contact sheet compare *physics* rather than composition.

**`reference_speed` is a visualisation parameter and must never be a
physics limit.** v1 normalised particle colour against the integrator's
velocity clamp while the integrator clamped to exactly that value, so
during the interesting phase of every run nearly every particle mapped to
1.0 and the fluid rendered as a flat white sheet.

`lighting` overrides the three-point rig (key/fill/rim direction, colour
and intensity). The defaults are shared across every scenario on purpose;
change them only for a scenario whose geometry genuinely needs it.

---

## A worked example: adding a new scenario

Suppose you want a **jet impinging on an inclined plate**. Nothing about
that exists in the solver, and nothing needs to.

1. **Domain**: a box, `y_max` and `x_max` open so the deflected sheet
   leaves rather than piling up.
2. **Material**: water; some surface tension, since a thin sheet is where
   it matters.
3. **Emitter**: a small cylinder aperture pointing down at 3 m/s.
4. **Obstacle**: a box, which you tilt by giving the heightfield a slope —
   or, more simply, a `heightfield` with `slope: [0.6, 0.0]` clipped to the
   plate's footprint.
5. **Metrics**: probes either side of the impingement point.
6. **Tier**: 1 if you are going to compare the sheet's spreading radius
   against a correlation; 2 if it is there to look at.

That is a new JSON file. No solver change, no rebuild.

If your idea *does* need a solver change — a rotating plate, say, or a
plate that the jet pushes — that is the architecture telling you it is a
Tier 3 extension point. `docs/architecture.md` lists them and where each
would attach.
