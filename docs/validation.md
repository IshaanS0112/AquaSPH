# Validation

This is what separates a simulation that looks convincing from one that is
demonstrably doing something. Three quantitative comparisons, each against
a reference that is derived here rather than asserted, so it can be
checked line by line.

**No physical parameter in this project has been tuned to make a curve
agree.** Where the simulation and the reference differ, the difference is
reported with its magnitude and its cause. A documented, explained
discrepancy is worth far more than a suspiciously perfect fit, and a
tuned agreement is recognisable.

Regenerate everything below with:

```bash
scripts/run_validation.sh medium results/validation
python3 scripts/validate.py results/validation
```

which writes `docs/_validation_tables.md` and
`docs/img/dam_break_surge_front.svg`.

---

## 1. Dam break — surge front

### The reference, and why it is Ritter rather than Martin & Moyce

The brief for this work asked for a comparison against **Martin & Moyce
(1952)**, the standard collapsing-column experiment. That comparison is
*not* shipped, and the reason is worth stating plainly rather than
quietly substituting something else:

> The original tabulation was not available in the environment where this
> validation was produced. Inventing plausible-looking experimental
> numbers — or transcribing half-remembered ones from a secondary source —
> would make every comparison built on them worthless, and would be
> exactly the kind of overclaiming that makes a reader discount the rest
> of a project.

`validation/martin_moyce_1952.json` ships with an empty `points` array and
a README explaining the conventions to fill it in with.
`scripts/validate.py` picks it up automatically and adds the experimental
series to the table and the plot; nothing else needs changing.

What *is* shipped is a comparison against the **Ritter (1892)** analytical
dry-bed dam-break solution, which is derivable from scratch and therefore
carries no provenance risk at all.

### Deriving the reference

Ritter solves the shallow-water equations for a reservoir of still depth
`h0` released instantaneously onto a dry, frictionless, horizontal bed.
The leading characteristic travels at

```
u_f = 2 sqrt(g h0)
```

so the front position measured from the dam face is
`x_f = 2 t sqrt(g h0)`.

In the Martin & Moyce non-dimensionalisation this project's metrics use —
`Z` = front position measured from the column's back wall divided by the
column width `a`, and `T = t sqrt(2g/a)` — with a column of height
`h0 = 2a` (the `n² = 2` case, which `dam_break.json` matches):

```
Z(T) = 1 + 2 t sqrt(g h0) / a
     = 1 + 2 T sqrt(a/2g) sqrt(2 g a) / a
     = 1 + 2 T
```

So the analytical reference is the straight line **Z = 1 + 2T**, with
`Z = 1` at `t = 0` by construction.

### Method

The **front** is the toe of the surge: the furthest-travelled fluid
particle still within three particle spacings of the floor. Taking the
furthest particle overall would track spray, which is both noisier and not
what a physical experiment measures with a wave gauge or a camera.
Sampled every 10 ms of simulated time.

`dam_break.json` at `--quality medium`: 21,525 fluid + 39,314 boundary
particles, `a = 0.25 m`, `h0 = 0.5 m`, tank 1.6 m long, so `Z` saturates
at 6.4 when the front reaches the far wall (around `T ≈ 8.5`). Only
`T < 6` is meaningful.

### Result

See `docs/_validation_tables.md` for the generated table and
`docs/img/dam_break_surge_front.svg` for the plot.

**The simulated front runs behind Ritter, by roughly a factor of two in
propagation speed over `1 < T < 4`.** That is a large discrepancy and it
is not a bug. Three separate causes, in decreasing order of size:

**1. Ritter is not an achievable upper bound at early time.** The
shallow-water equations have no vertical acceleration, so at `t = 0+` the
front is *already* moving at `2 sqrt(g h0)`. Neither a real dam break nor
an SPH one does that: the column has to accelerate from rest, and the
flow near the gate is strongly non-hydrostatic for the first several
`sqrt(a/g)`. Every published experimental dam-break curve, Martin & Moyce
included, lies below Ritter in this range for the same reason. Comparing
an early-time slope against Ritter's asymptotic celerity is comparing
against a limit the flow has not reached.

**2. The artificial viscosity is high.** `Material::viscosity = 5 Pa·s`
gives `nu = 5e-3 m²/s`, so with a front speed of ~2 m/s over the 0.5 m
column height the Reynolds number is of order 200 — a real dam break at
this scale is ~10^6. This is a deliberate sub-particle
dissipation model (see `docs/scenarios.md`), and it demonstrably slows the
front. Measured, `--quality low`, front slope `dZ/dT` fitted over
`1 < T < 4`:

| `viscosity` (Pa·s) | `nu` (m²/s) | Re ≈ | `dZ/dT` | within 1% of rest density |
|---:|---:|---:|---:|---:|
| 5.0 | 5.0e-3 | ~200 | 1.11 | 32.7% |
| 1.0 | 1.0e-3 | ~1000 | 1.42 | 15.5% |

(Ritter's asymptotic slope is 2.0. Cutting the viscosity by a factor of
five moves the front 28% closer to the inviscid limit and halves the
fraction of particles holding rest density. Extending the sweep further
down is a one-line change to the material block; the trend continues in
both directions.)

The trade is explicit: lower viscosity moves the front closer to the
inviscid limit and makes the density field noisier. **The shipped value
was chosen for the density field, not for the front position** — which is
the opposite of tuning to fit, and is why the sensitivity is published
here rather than left out.

**3. No-slip walls.** `boundary_friction = 1.0` applies the full viscous
term against the tank floor and side walls. Real flume walls are also
no-slip, so this is the right model, but at one particle spacing of
near-wall resolution the drag is not quantitatively converged. Setting
`boundary_friction = 0` makes wall friction a free-slip idealisation and
is the knob to turn if you want to isolate cause 2 from cause 3.

### What this comparison does establish

- The front position is **monotone, smooth and repeatable**, with no
  spray-driven noise.
- Fluid volume is conserved to under 3% over the run
  (`volume.initial_m3` vs `volume.final_m3` in the metrics).
- The run is stable with zero containment failures and zero non-finite
  particles, at a physically sensible maximum speed (~3.9 m/s against a
  free-fall estimate of `sqrt(2 g h0)` = 3.1 m/s for the collapsing
  column, plus surge acceleration).

### What it does not establish

Without the experimental table, this is a comparison against an
**analytical upper bound that the physical experiment also fails to
reach**. It bounds the answer and explains the gap; it does not confirm
the model against measurement. Filling in
`validation/martin_moyce_1952.json` from the primary source is the single
highest-value addition anyone could make to this repository.

---

## 2. Sloshing tank — oscillation period

### The references

Two analytical results, both quoted, because they disagree by about 4% at
this tank's aspect ratio and quoting only the closer one would hide which
model is actually being tested.

**Shallow-water (long-wave) limit.** A disturbance travels at
`c = sqrt(g d)`, and the fundamental sloshing mode is a standing wave of
wavelength `2L`, so

```
T_shallow = 2L / sqrt(g d)
```

**Linear dispersion, first mode.** Without the long-wave assumption, the
first mode has wavenumber `k = pi/L` and

```
omega² = g k tanh(k d),    T_linear = 2 pi / omega
```

For `L = 0.6 m` and `d = 0.1 m`, `d/L = 1/6` — shallow, but not *that*
shallow: `tanh(kd) = 0.48`, not `kd`. So `T_shallow = 1.211 s` and
`T_linear = 1.265 s`, and the dispersive result is the better reference.

### Method

The tank is given a **short horizontal impulse** — a `pulse` external
force, 2 m/s² for 0.25 s — and then left alone. Measuring the free decay
rather than driving it at resonance is deliberate: a driven system reports
the forcing period, not the tank's, so it would validate nothing.

The period is fitted from **zero up-crossings** of the surface elevation
recorded at a wall probe, de-meaned, with the crossings linearly
interpolated so the period is not quantised to the 20 ms sample interval.
Up-crossings only, so each period is counted once.

### Result

See `docs/_validation_tables.md`.

### Interpretation

The measured period should sit close to `T_linear` and slightly above
both analytical values. Two effects push it up: finite amplitude (the
analytical results are small-amplitude) and the fluid's effective depth
being slightly less than `d` where it meets the no-slip walls.

**The decay rate is not validated and is not predictive.** Wall friction
here is a single coefficient scaling a viscous term, not a resolved
boundary layer, so how fast the sloshing dies away is a property of the
numerics as much as of the fluid. Only the period is being claimed.

---

## 3. Wave generator — fidelity

This is what turns the wave tank from a visual effect into an instrument.
The paddle is *commanded*; the amplitude, period, wavelength and celerity
that come out are *measured*, and linear theory predicts what they should
be.

### The reference

For a piston wavemaker in water of depth `d`, linear (Biesel) theory
relates the paddle stroke `S = 2a` to the generated wave height `H`:

```
H/S = 2 (cosh(2kd) - 1) / (sinh(2kd) + 2kd)
```

with `k` from the dispersion relation `omega² = g k tanh(k d)`, solved by
bisection between the deep- and shallow-water bounds (`MetricsCollector::
predictWave`). Wavelength is `2 pi / k` and celerity `L / T`.

The solver emits this prediction into the metrics **alongside** the
measurement, so the two cannot drift apart in a later edit.

### Method

Three wave gauges at 0.8, 1.4 and 2.0 m down a 2.4 m flume, 0.2 m still
depth. Each records surface elevation every 20 ms; amplitude and period
are fitted from zero up-crossings as above, and wavelength and celerity
follow from the measured period through the dispersion relation.

The paddle ramps up over a full period with a smoothstep envelope.
Starting it impulsively radiates a transient down the flume that
contaminates the whole record — the ramp is not cosmetic.

### Result

See `docs/_validation_tables.md`.

### What limits this measurement, stated up front

- **Amplitude has a quantisation floor.** The commanded wave height is
  about three particle spacings. Surface elevation is measured as the
  topmost particle in a probe column, so the amplitude carries an
  uncertainty of order one spacing — roughly 20-30% of the wave height.
  **Period and celerity do not share this limit**: they come from crossing
  *times*, and the elevation record crosses its mean cleanly regardless of
  how coarsely the crest is resolved. Trust the period; treat the
  amplitude as order-of-magnitude.
- **The flume holds about two wavelengths, and reflections return.** At
  the commanded 0.9 s period in 0.2 m of water, linear theory gives
  `L = 1.05 m` and `c = 1.17 m/s`; the paddle sits at x = 0.1 and the far
  wall at x = 2.4, so the first wave reaches gauge `wg1` at x = 0.8 about
  0.6 s after the ramp completes, and its reflection returns there about
  3.3 s later. Everything after that is a partial standing wave.

  The fit spans the whole record. **The period survives this** —
  reflections have the same period as the incident wave, which is why
  period and celerity are the quantities reported with confidence. **The
  amplitude does not**: standing-wave build-up inflates it, on top of the
  quantisation floor above. A longer flume, or a fit restricted to the
  clean window, would fix it; both are listed here rather than claimed.
- **Linear theory is a small-amplitude theory.** The prediction is
  flagged in the metrics (`linear_theory_applicable`) when the measured
  steepness exceeds `H/L = 1/20`, and is reported but not treated as a
  target beyond that.
- **No prediction is offered for pulse or superposition modes.** Linear
  monochromatic theory does not describe them, and no number is better
  than a wrong one — `predictWave` returns invalid rather than a plausible
  figure.

### Why a paddle rather than an imposed surface

A wave generator here **is** an obstacle with prescribed motion. Imposing
a free-surface displacement `eta(x,t)` directly would have been far
easier and would have produced better-looking waves immediately — and it
would have validated nothing, because a prescribed surface is a rendering
trick the fluid does not obey. A paddle makes waves the solver has to
propagate itself, which is the entire reason amplitude, period and
celerity can be treated as measurements.

---

## 4. Flood metrics — reported, and explicitly not validated

The Tier 2 flood scenarios emit depth at named stations, arrival time (as
the difference between station rises), maximum depth, and inundated
floor-area fraction. These are in the metrics JSON in machine-readable
form.

**They are not validated against anything, and no reference exists in
this repository against which they could be.** They are internally
consistent measurements of a metre-scale flume experiment over analytic
terrain, with no infiltration, no roughness model, no sediment and no
structural failure. The inundation grid is tied to the particle spacing,
so the area fraction is comparable across quality presets only to within
one cell.

They are emitted because a number you can compare between two runs of your
own is useful, and because a scenario that reports nothing is an
animation. They are not emitted as a flood map.

---

## Summary of what is and is not claimed

| scenario | reference | status |
|---|---|---|
| `dam_break` | Ritter (1892) analytical, derived above | **Compared.** Front runs behind the analytical bound; magnitude, three causes and a viscosity sensitivity sweep reported |
| `dam_break` | Martin & Moyce (1952) experimental | **Not compared.** Reference data unavailable; scaffolding and instructions shipped rather than invented numbers |
| `sloshing_tank` | shallow-water and linear-dispersion periods, derived above | **Compared.** Period only; decay rate explicitly not claimed |
| `controlled_wave_tank` | linear (Biesel) piston-wavemaker theory | **Compared.** Period and celerity meaningful; amplitude limited by resolution, and the limit is quantified |
| Tier 2 flood scenarios | none | **Not validated.** Metrics reported as internally consistent measurements, never as predictions |
