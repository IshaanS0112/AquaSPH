#!/usr/bin/env python3
"""Compare AquaSPH's measured output against analytical and experimental
references, and write docs/validation.md plus an SVG plot.

Reads the metrics JSON that `aquasph --metrics` produces. Nothing here
touches the solver: validation is strictly downstream of physics, and no
number below is tuned to make a curve agree.

Usage:
    scripts/validate.py [results-dir]        (default: results/validation)
"""
import json
import math
import os
import sys

RESULTS = sys.argv[1] if len(sys.argv) > 1 else "results/validation"
DOCS = "docs/validation.md"
PLOT = "docs/img/dam_break_surge_front.svg"
G = 9.81


def load(name):
    path = os.path.join(RESULTS, name + ".json")
    if not os.path.exists(path):
        return None
    with open(path) as f:
        return json.load(f)


def fmt(x, nd=4):
    return "n/a" if x is None else f"{x:.{nd}f}"


def rel_error(measured, reference):
    if reference in (None, 0):
        return None
    return (measured - reference) / reference * 100.0


# ---------------------------------------------------------------------
# Dam break: surge front against the Ritter (1892) analytical solution.
#
# Ritter solves the shallow-water equations for an instantaneously
# released reservoir of still depth h0 on a dry, frictionless, horizontal
# bed. The leading characteristic travels at u_f = 2*sqrt(g*h0), so the
# front position measured from the dam face is x_f = 2t*sqrt(g*h0).
#
# In the Martin & Moyce non-dimensionalisation used by this project's
# metrics -- Z = (front position from the column's back wall) / a, and
# T = t*sqrt(2g/a) -- with a column of height h0 = 2a:
#
#   Z(T) = 1 + 2t*sqrt(g*h0)/a
#        = 1 + 2*T*sqrt(a/(2g))*sqrt(2ga)/a
#        = 1 + 2T
#
# Derived here rather than cited so it can be checked line by line.
# ---------------------------------------------------------------------
def ritter_Z(T):
    return 1.0 + 2.0 * T


def dam_break_section():
    m = load("dam_break")
    if not m or not m.get("surge_front"):
        return "_No dam_break metrics found; run scripts/run_validation.sh first._\n", []

    pts = [(p["T"], p["Z"]) for p in m["surge_front"] if p["T"] is not None]
    pts = [(t, z) for (t, z) in pts if t is not None and z is not None]
    pts.sort()

    exp_path = "validation/martin_moyce_1952.json"
    exp = []
    if os.path.exists(exp_path):
        with open(exp_path) as f:
            exp = json.load(f).get("points", [])

    rows = []
    for target in [0.5, 1.0, 1.5, 2.0, 2.5, 3.0]:
        near = min(pts, key=lambda p: abs(p[0] - target), default=None)
        if near is None or abs(near[0] - target) > 0.25:
            continue
        T, Z = near
        r = ritter_Z(T)
        rows.append((T, Z, r, (Z - r) / r * 100.0))

    out = []
    out.append("| T = t sqrt(2g/a) | Z measured | Z Ritter (analytic) | difference |")
    out.append("|---:|---:|---:|---:|")
    for T, Z, r, e in rows:
        out.append(f"| {T:.2f} | {Z:.3f} | {r:.3f} | {e:+.1f}% |")
    return "\n".join(out) + "\n", (pts, exp, rows)


# ---------------------------------------------------------------------
# Sloshing: measured oscillation period against two analytical results.
#
#   shallow-water (long-wave limit):  T = 2L / sqrt(g d)
#   linear dispersion, first mode:    omega^2 = g k tanh(k d), k = pi/L
#
# The shallow-water form is the one the brief asks for; the dispersive
# form is also reported because at d/L = 1/6 the tank is NOT deep in the
# long-wave sense, and the two differ by about 4%. Quoting only the
# closer one would be a way of hiding which model is actually being
# tested.
# ---------------------------------------------------------------------
def sloshing_section(tank_length=0.6, depth=0.1):
    m = load("sloshing_tank")
    shallow = 2.0 * tank_length / math.sqrt(G * depth)
    k = math.pi / tank_length
    omega = math.sqrt(G * k * math.tanh(k * depth))
    dispersive = 2.0 * math.pi / omega

    if not m:
        return ("_No sloshing_tank metrics found._\n", shallow, dispersive, None)

    # The CENTROID is the primary instrument here, not the wall probes. A
    # probe reads the topmost particle in a column, so its floor is one
    # particle spacing -- and a standing wave small enough for linear
    # theory to apply is smaller than that. The centre of mass averages
    # over every fluid particle and has no such floor. The probes are
    # reported alongside precisely so the difference is visible.
    osc = (m.get("centroid") or {}).get("oscillation") or {}
    measured = osc.get("period")
    cycles = osc.get("cycles_counted", 0)

    out = [
        "| quantity | value |",
        "|---|---:|",
        f"| tank length L | {tank_length:.3f} m |",
        f"| still depth d | {depth:.3f} m |",
        f"| shallow-water period `2L/sqrt(gd)` | {shallow:.3f} s |",
        f"| linear-dispersion first mode | {dispersive:.3f} s |",
        f"| **measured** (fluid centre of mass, {cycles} cycles) | **{fmt(measured, 3)} s** |",
    ]
    if measured:
        out.append(f"| error vs shallow-water | **{rel_error(measured, shallow):+.1f}%** |")
        out.append(f"| error vs linear dispersion | **{rel_error(measured, dispersive):+.1f}%** |")

    out.append("")
    out.append("Surface probes, for comparison — and as evidence of why they are "
                "not the instrument used here:")
    out.append("")
    out.append("| probe | amplitude (m) | period (s) | crossings counted |")
    out.append("|---|---:|---:|---:|")
    for probe in m.get("probes", []):
        meas = probe.get("measured")
        if not meas:
            continue
        out.append(f"| {probe['name']} | {meas['amplitude']:.4f} | "
                   f"{meas['period']:.3f} | {meas['waves_counted']} |")
    return "\n".join(out) + "\n", shallow, dispersive, measured


# ---------------------------------------------------------------------
# Wave generator: measured wave train against linear (Biesel) piston
# wavemaker theory, which the solver itself emits alongside the
# measurement so the two cannot drift apart.
# ---------------------------------------------------------------------
def wave_section():
    m = load("controlled_wave_tank")
    if not m:
        return "_No controlled_wave_tank metrics found._\n"

    preds = [p for p in m.get("wave_predictions", []) if p.get("valid")]
    pred = preds[0] if preds else None

    out = ["| probe | x (m) | amplitude (m) | period (s) | celerity (m/s) | waves |",
           "|---|---:|---:|---:|---:|---:|"]
    for probe in m.get("probes", []):
        meas = probe.get("measured")
        if not meas:
            continue
        pos = probe.get("position", [0, 0, 0])
        out.append(f"| {probe['name']} | {pos[0]:.2f} | {meas['amplitude']:.4f} | "
                   f"{meas['period']:.3f} | {fmt(meas.get('celerity'), 3)} | "
                   f"{meas['waves_counted']} |")

    if pred:
        out.append(f"| **linear theory** | -- | {pred['height'] / 2.0:.4f} | "
                   f"{pred['period']:.3f} | {pred['celerity']:.3f} | -- |")
        out.append("")
        out.append(f"Commanded paddle period {pred['period']:.3f} s, still depth "
                   f"{pred['depth']:.3f} m, predicted wavelength "
                   f"{pred['wavelength']:.3f} m, predicted steepness H/L = "
                   f"{pred['steepness']:.4f} "
                   f"({'within' if pred['linear_theory_applicable'] else 'BEYOND'} "
                   "the small-amplitude range where linear theory applies).")
    return "\n".join(out) + "\n"


def write_plot(pts, exp):
    """Hand-written SVG. No plotting library: one more dependency for one
    chart, and an SVG in the repository diffs and renders on GitHub."""
    os.makedirs(os.path.dirname(PLOT), exist_ok=True)
    W, H, PAD = 640, 400, 58
    if not pts:
        return

    # The tank is finite: once the surge reaches the far wall, Z stops
    # growing and the record says nothing further about propagation. The
    # plot is cut just past that point rather than showing a long flat
    # tail that would make the agreement look better than it is.
    zlimit = max(p[1] for p in pts)
    saturated = [t for (t, z) in pts if z >= zlimit * 0.985]
    tmax = min(saturated) * 1.05 if saturated else max(p[0] for p in pts)
    tmax = max(tmax, 3.0)
    zmax = max(2.0, zlimit * 1.05, ritter_Z(tmax) * 0.55)

    def sx(T):
        return PAD + (W - 2 * PAD) * T / tmax

    def sy(Z):
        return H - PAD - (H - 2 * PAD) * Z / zmax

    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
         f'viewBox="0 0 {W} {H}" font-family="system-ui,sans-serif" font-size="12">',
         f'<rect width="{W}" height="{H}" fill="#0f1115"/>']

    for i in range(7):
        T = tmax * i / 6
        s.append(f'<line x1="{sx(T):.1f}" y1="{PAD}" x2="{sx(T):.1f}" y2="{H-PAD}" '
                 f'stroke="#22262e"/>')
        s.append(f'<text x="{sx(T):.1f}" y="{H-PAD+18}" fill="#8b93a1" '
                 f'text-anchor="middle">{T:.1f}</text>')
    for i in range(7):
        Z = zmax * i / 6
        s.append(f'<line x1="{PAD}" y1="{sy(Z):.1f}" x2="{W-PAD}" y2="{sy(Z):.1f}" '
                 f'stroke="#22262e"/>')
        s.append(f'<text x="{PAD-8}" y="{sy(Z)+4:.1f}" fill="#8b93a1" '
                 f'text-anchor="end">{Z:.1f}</text>')

    ritter = " ".join(f"{sx(tmax*i/100):.1f},{sy(min(ritter_Z(tmax*i/100), zmax)):.1f}"
                       for i in range(101))
    s.append(f'<polyline points="{ritter}" fill="none" stroke="#e0a458" '
             f'stroke-width="2" stroke-dasharray="7 5"/>')

    measured = " ".join(f"{sx(t):.1f},{sy(min(z, zmax)):.1f}" for t, z in pts if t <= tmax)
    s.append(f'<polyline points="{measured}" fill="none" stroke="#4fc3d9" stroke-width="2.5"/>')

    for T, Z in exp:
        s.append(f'<circle cx="{sx(T):.1f}" cy="{sy(min(Z, zmax)):.1f}" r="3.5" '
                 f'fill="none" stroke="#c8d3e0" stroke-width="1.6"/>')

    # The far wall, so a reader can see where the measurement stops being
    # about propagation and starts being about the tank.
    if zlimit <= zmax:
        s.append(f'<line x1="{PAD}" y1="{sy(zlimit):.1f}" x2="{W-PAD}" y2="{sy(zlimit):.1f}" '
                 f'stroke="#5a6472" stroke-width="1" stroke-dasharray="3 4"/>')
        s.append(f'<text x="{W-PAD-4:.0f}" y="{sy(zlimit)-6:.1f}" fill="#5a6472" '
                 f'text-anchor="end" font-size="11">far wall</text>')

    s.append(f'<text x="{W/2:.0f}" y="{H-12}" fill="#c8d3e0" text-anchor="middle">'
             f'T = t sqrt(2g/a)</text>')
    s.append(f'<text x="16" y="{H/2:.0f}" fill="#c8d3e0" text-anchor="middle" '
             f'transform="rotate(-90 16 {H/2:.0f})">Z = x_front / a</text>')
    s.append(f'<text x="{PAD}" y="{PAD-22}" fill="#c8d3e0" font-size="13">'
             f'Dam-break surge front</text>')
    s.append(f'<line x1="{W-235}" y1="{PAD-14}" x2="{W-210}" y2="{PAD-14}" '
             f'stroke="#4fc3d9" stroke-width="2.5"/>')
    s.append(f'<text x="{W-185}" y="{PAD-10}" fill="#8b93a1">AquaSPH</text>')
    s.append(f'<line x1="{W-235}" y1="{PAD+2}" x2="{W-210}" y2="{PAD+2}" '
             f'stroke="#e0a458" stroke-width="2" stroke-dasharray="7 5"/>')
    s.append(f'<text x="{W-205}" y="{PAD+6}" fill="#8b93a1">Ritter (1892), analytic</text>')
    if exp:
        s.append(f'<circle cx="{W-223}" cy="{PAD+18}" r="3.5" fill="none" '
                 f'stroke="#c8d3e0" stroke-width="1.6"/>')
        s.append(f'<text x="{W-205}" y="{PAD+22}" fill="#8b93a1">Martin &amp; Moyce (1952)</text>')
    s.append('</svg>')

    with open(PLOT, "w") as f:
        f.write("\n".join(s) + "\n")
    print(f"wrote {PLOT}")


def main():
    dam_table, dam_data = dam_break_section()
    slosh_table, shallow, dispersive, slosh_measured = sloshing_section()
    wave_table = wave_section()

    if dam_data:
        pts, exp, _ = dam_data
        write_plot(pts, exp)

    print("\n=== dam break ===\n" + dam_table)
    print("=== sloshing ===\n" + slosh_table)
    print("=== waves ===\n" + wave_table)

    with open("docs/_validation_tables.md", "w") as f:
        f.write("<!-- generated by scripts/validate.py -- do not edit by hand -->\n\n")
        f.write("## Dam break vs Ritter\n\n" + dam_table + "\n")
        f.write("## Sloshing period\n\n" + slosh_table + "\n")
        f.write("## Wave generator fidelity\n\n" + wave_table + "\n")
    print("wrote docs/_validation_tables.md")


if __name__ == "__main__":
    main()
