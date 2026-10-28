"""Render an AquaSPH particle snapshot with Blender Cycles.

Builds the scene from the snapshot alone (domain, open faces, obstacles and terrain are in its
header, docs/snapshots.md), turns the particles into a water surface through OpenVDB, and
renders it in a dark studio that matches the console's Graphite theme.

    # with the bpy module (pip install bpy==4.5.15, Python 3.11)
    python render/blender/render_scene.py --snapshot run.aqsnap --scenario configs/scenarios/dam_break.json \
        --out dam_break.jpg --width 1600 --height 900 --samples 128

    # with a Blender install
    blender -b -P render/blender/render_scene.py -- --snapshot run.aqsnap --out dam_break.jpg
"""
import argparse
import json
import math
import struct
import sys
import time

import bpy
import numpy as np

# --------------------------------------------------------------------------------------------
# Snapshot reader (mirrors src/io/Snapshot.cpp)

SHAPE_BOX, SHAPE_SPHERE, SHAPE_CYLINDER, SHAPE_HEIGHTFIELD = 0, 1, 2, 3


def read_snapshot(path):
    data = open(path, "rb").read()
    if data[:8] != b"AQSNAP1\n":
        raise SystemExit(f"{path}: not an AquaSPH snapshot")
    version, header_size, frame_count, flags = struct.unpack_from("<4I", data, 8)
    f = lambda off, n: np.frombuffer(data, "<f4", n, off).astype(np.float64)
    h = {
        "flags": flags,
        "domain_min": f(24, 3), "domain_max": f(36, 3),
        "qmin": f(48, 3), "qmax": f(60, 3),
        "radius": struct.unpack_from("<f", data, 72)[0],
        "vref": struct.unpack_from("<f", data, 76)[0],
        "spacing": struct.unpack_from("<f", data, 80)[0],
        "h": struct.unpack_from("<f", data, 84)[0],
        "faces": list(data[88:94]),
    }
    n_mat, = struct.unpack_from("<H", data, 94)
    n_obs, = struct.unpack_from("<I", data, 96)
    pos = 100
    mats = []
    for _ in range(n_mat):
        ln = data[pos]; pos += 1
        name = data[pos:pos + ln].decode(); pos += ln
        absorption = f(pos, 3); pos += 12
        rho, = struct.unpack_from("<f", data, pos); pos += 4
        mats.append({"name": name, "absorption": absorption, "rest_density": rho})
    obstacles = []
    for _ in range(n_obs):
        typ, axis, ln = data[pos], data[pos + 1], data[pos + 2]; pos += 3
        name = data[pos:pos + ln].decode(); pos += ln
        vals = f(pos, 11); pos += 44
        o = {"type": typ, "axis": axis, "name": name, "min": vals[0:3], "max": vals[3:6],
             "center": vals[6:9], "radius": vals[9], "half_length": vals[10]}
        if typ == SHAPE_HEIGHTFIELD:
            hv = f(pos, 7); pos += 28
            nb, = struct.unpack_from("<I", data, pos); pos += 4
            o.update(base=hv[0], origin=hv[1:3], slope=hv[3:5], min_h=hv[5], max_h=hv[6],
                     bumps=f(pos, 4 * nb).reshape(nb, 4)); pos += 16 * nb
        obstacles.append(o)
    h["materials"], h["obstacles"] = mats, obstacles

    frames = []
    pos = header_size
    with_mat = bool(flags & 1)
    while pos + 16 <= len(data):
        if frame_count != 0xFFFFFFFF and len(frames) >= frame_count:
            break
        magic, t, n, _ = struct.unpack_from("<IfII", data, pos)
        size = 16 + 12 * n_obs + 7 * n + (n if with_mat else 0)
        size = (size + 3) & ~3
        if magic != 0x4D415246 or pos + size > len(data):
            break
        off = pos + 16
        offsets = f(off, 3 * n_obs).reshape(n_obs, 3); off += 12 * n_obs
        q = np.frombuffer(data, "<u2", 3 * n, off).reshape(n, 3).astype(np.float64); off += 6 * n
        speed = np.frombuffer(data, "u1", n, off).astype(np.float64) * h["vref"] / 255.0
        xyz = h["qmin"] + (h["qmax"] - h["qmin"]) * q / 65535.0
        frames.append({"time": t, "positions": xyz, "speed": speed, "offsets": offsets})
        pos += size
    return h, frames


# --------------------------------------------------------------------------------------------
# Geometry helpers. The solver is y-up, Blender is z-up: (x, y, z) -> (x, -z, y).

def to_b(v):
    return (float(v[0]), float(-v[2]), float(v[1]))


def box_object(name, lo, hi, material, bevel=0.0):
    lo_b, hi_b = np.array(to_b(lo)), np.array(to_b(hi))
    mn, mx = np.minimum(lo_b, hi_b), np.maximum(lo_b, hi_b)
    bpy.ops.mesh.primitive_cube_add(location=tuple((mn + mx) / 2))
    ob = bpy.context.active_object
    ob.name = name
    ob.scale = tuple((mx - mn) / 2)
    bpy.ops.object.transform_apply(scale=True)
    if bevel > 0:
        m = ob.modifiers.new("bevel", "BEVEL")
        m.width, m.segments, m.limit_method = bevel, 3, "ANGLE"
    ob.data.materials.append(material)
    for p in ob.data.polygons:
        p.use_smooth = bevel > 0
    return ob


# --------------------------------------------------------------------------------------------
# Materials

def principled(name, color, roughness, **inputs):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = (*color, 1.0)
    b.inputs["Roughness"].default_value = roughness
    for k, v in inputs.items():
        b.inputs[k].default_value = v
    return m


def water_material(absorption, scale):
    m = principled("Water", (1.0, 1.0, 1.0), 0.03, **{"IOR": 1.333, "Transmission Weight": 1.0})
    nt = m.node_tree
    vol = nt.nodes.new("ShaderNodeVolumeAbsorption")
    # Cycles absorbs density * (1 - colour) per metre. The scenario's coefficients are physical
    # (1/m); `scale` tints lab-scale water enough to read on screen.
    a = np.asarray(absorption) * scale
    density = max(float(a.max()), 1e-3)
    vol.inputs["Density"].default_value = density
    vol.inputs["Color"].default_value = (*(1.0 - a / density), 1.0)
    nt.links.new(vol.outputs["Volume"], nt.nodes["Material Output"].inputs["Volume"])
    return m


def floor_material(grid_step):
    m = principled("Floor", (0.035, 0.038, 0.043), 0.55)
    nt = m.node_tree
    coord = nt.nodes.new("ShaderNodeTexCoord")
    sep = nt.nodes.new("ShaderNodeSeparateXYZ")
    nt.links.new(coord.outputs["Object"], sep.inputs[0])
    lines = []
    for axis in ("X", "Y"):
        div = nt.nodes.new("ShaderNodeMath"); div.operation = "DIVIDE"; div.inputs[1].default_value = grid_step
        frac = nt.nodes.new("ShaderNodeMath"); frac.operation = "PINGPONG"; frac.inputs[1].default_value = 0.5
        step = nt.nodes.new("ShaderNodeMath"); step.operation = "LESS_THAN"; step.inputs[1].default_value = 0.012
        nt.links.new(sep.outputs[axis], div.inputs[0])
        nt.links.new(div.outputs[0], frac.inputs[0])
        nt.links.new(frac.outputs[0], step.inputs[0])
        lines.append(step)
    both = nt.nodes.new("ShaderNodeMath"); both.operation = "MAXIMUM"
    nt.links.new(lines[0].outputs[0], both.inputs[0])
    nt.links.new(lines[1].outputs[0], both.inputs[1])
    mix = nt.nodes.new("ShaderNodeMix"); mix.data_type = "RGBA"
    mix.inputs["A"].default_value = (0.035, 0.038, 0.043, 1)
    mix.inputs["B"].default_value = (0.075, 0.08, 0.088, 1)
    nt.links.new(both.outputs[0], mix.inputs["Factor"])
    nt.links.new(mix.outputs["Result"], nt.nodes["Principled BSDF"].inputs["Base Color"])
    return m


# --------------------------------------------------------------------------------------------
# Scene

def _openvdb():
    import glob
    import os
    try:
        import openvdb
    except ImportError:
        local = bpy.utils.resource_path("LOCAL")
        sys.path.extend(glob.glob(os.path.join(local, "python", "lib", "python3*", "site-packages")))
        import openvdb
    return openvdb


def density_field(pb, s, voxel_frac=0.35, kernel_frac=2.5):
    """SPH-style smoothed density on a voxel grid, normalised so the bulk of a resting lattice
    reads 1.0. This is the field splashsurf reconstructs from, rather than a union of spheres:
    it closes the gaps between particles instead of leaving them as cavities."""
    vs, R = voxel_frac * s, kernel_frac * s
    lo = pb.min(axis=0) - R - 2 * vs
    dims = np.ceil((pb.max(axis=0) + R + 2 * vs - lo) / vs).astype(np.int64) + 1
    base = np.floor((pb - lo) / vs).astype(np.int64)
    rv = int(math.ceil(R / vs))
    rng = np.arange(-rv, rv + 2)
    offs = np.stack(np.meshgrid(rng, rng, rng, indexing="ij"), axis=-1).reshape(-1, 3)
    grid = np.zeros(int(dims.prod()), dtype=np.float64)
    for chunk in np.array_split(offs, max(1, len(offs) // 48)):
        idx = base[None, :, :] + chunk[:, None, :]
        d2 = (((lo + idx * vs) - pb[None, :, :]) ** 2).sum(axis=-1)
        q2 = d2 / (R * R)
        mask = q2 < 1.0
        w = (1.0 - q2[mask]) ** 3  # poly6
        lin = np.ravel_multi_index((idx[..., 0][mask], idx[..., 1][mask], idx[..., 2][mask]), tuple(dims))
        grid += np.bincount(lin, weights=w, minlength=grid.size)
    lat = np.arange(-3, 4) * s
    L = np.stack(np.meshgrid(lat, lat, lat, indexing="ij"), axis=-1).reshape(-1, 3)
    q2 = (L ** 2).sum(axis=1) / (R * R)
    bulk = float((((1.0 - q2) ** 3) * (q2 < 1.0)).sum())
    phi = (grid / bulk).reshape(tuple(dims)).astype(np.float32)
    at_particles = phi[tuple(np.clip(np.rint((pb - lo) / vs).astype(np.int64), 0, dims - 1).T)]
    return phi, lo, vs, at_particles


def fluid_surface(name, points, spacing, material, iso=0.5):
    """Particles -> density field -> OpenVDB iso-surface. Returns the mesh object and a mask of
    the particles the surface does not enclose (spray)."""
    pb = np.stack([points[:, 0], -points[:, 2], points[:, 1]], axis=1)
    phi, lo, vs, at_particles = density_field(pb, spacing)
    vdb = _openvdb()
    g = vdb.FloatGrid()
    g.copyFromArray(phi)
    g.transform = vdb.createLinearTransform(voxelSize=vs)
    verts, quads = g.convertToQuads(isovalue=iso)
    verts = np.asarray(verts, dtype=np.float64) + lo
    quads = np.asarray(quads, dtype=np.int64)
    mesh = bpy.data.meshes.new(name)
    mesh.vertices.add(len(verts))
    mesh.vertices.foreach_set("co", verts.astype(np.float32).ravel())
    mesh.loops.add(len(quads) * 4)
    mesh.loops.foreach_set("vertex_index", quads.astype(np.int32).ravel())
    mesh.polygons.add(len(quads))
    mesh.polygons.foreach_set("loop_start", (np.arange(len(quads)) * 4).astype(np.int32))
    mesh.polygons.foreach_set("loop_total", np.full(len(quads), 4, dtype=np.int32))
    mesh.update(calc_edges=True)
    mesh.validate()
    # Inside is high density, so the extracted quads face inward; flip if the volume is negative.
    v = verts[quads]
    signed = np.einsum("ij,ij->i", v[:, 0], np.cross(v[:, 1] - v[:, 0], v[:, 2] - v[:, 0])).sum()
    if signed < 0:
        mesh.flip_normals()
    mesh.shade_smooth()
    ob = bpy.data.objects.new(name, mesh)
    ob.data.materials.append(material)
    bpy.context.scene.collection.objects.link(ob)
    sm = ob.modifiers.new("smooth", "CORRECTIVE_SMOOTH")
    sm.factor, sm.iterations, sm.use_only_smooth = 0.5, 10, True
    return ob, at_particles < iso


def spray(points, spacing, material):
    """Isolated particles render as droplets rather than as lumps of surface."""
    if len(points) == 0:
        return None
    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=2, radius=spacing * 0.26)
    drop = bpy.context.active_object
    drop.name = "SprayDroplet"
    drop.data.materials.append(material)
    for p in drop.data.polygons:
        p.use_smooth = True
    drop.hide_render = True
    drop.hide_viewport = True
    mesh = bpy.data.meshes.new("Spray")
    pb = np.stack([points[:, 0], -points[:, 2], points[:, 1]], axis=1)
    mesh.vertices.add(len(pb))
    mesh.vertices.foreach_set("co", pb.astype(np.float32).ravel())
    ob = bpy.data.objects.new("Spray", mesh)
    bpy.context.scene.collection.objects.link(ob)
    ob.instance_type = "VERTS"
    drop.parent = ob
    drop.hide_render = False
    return ob


def heightfield(o, offset, material, res=160):
    lo, hi = o["min"], o["max"]
    xs = np.linspace(lo[0], hi[0], res)
    zs = np.linspace(lo[2], hi[2], max(8, int(res * (hi[2] - lo[2]) / max(hi[0] - lo[0], 1e-6))))
    X, Z = np.meshgrid(xs, zs, indexing="ij")
    Y = o["base"] + o["slope"][0] * (X - o["origin"][0]) + o["slope"][1] * (Z - o["origin"][1])
    for a, cx, cz, s in o["bumps"]:
        if s > 0:
            Y = Y + a * np.exp(-((X - cx) ** 2 + (Z - cz) ** 2) / (2 * s * s))
    Y = np.clip(Y, o["min_h"], o["max_h"])
    Y = np.clip(Y, lo[1], hi[1])
    nx, nz = X.shape
    top = np.stack([X, Y, Z], axis=-1).reshape(-1, 3) + offset
    bottom = top.copy()
    bottom[:, 1] = lo[1] + offset[1]
    verts = np.concatenate([top, bottom])
    faces = []
    for i in range(nx - 1):
        for j in range(nz - 1):
            a, b, c, d = i * nz + j, (i + 1) * nz + j, (i + 1) * nz + j + 1, i * nz + j + 1
            faces.append((a, d, c, b))
    base = nx * nz
    edge = [(i, 0) for i in range(nx)] + [(nx - 1, j) for j in range(nz)] + \
           [(i, nz - 1) for i in reversed(range(nx))] + [(0, j) for j in reversed(range(nz))]
    for (i0, j0), (i1, j1) in zip(edge, edge[1:]):
        t0, t1 = i0 * nz + j0, i1 * nz + j1
        if t0 != t1:
            faces.append((t0, t1, base + t1, base + t0))
    mesh = bpy.data.meshes.new(o["name"])
    mesh.from_pydata([to_b(v) for v in verts], [], faces)
    mesh.update()
    for p in mesh.polygons:
        p.use_smooth = True
    ob = bpy.data.objects.new(o["name"], mesh)
    ob.data.materials.append(material)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def build_scene(h, frame, camera_spec, args):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    dmin, dmax = h["domain_min"], h["domain_max"]
    extent = dmax - dmin
    size = float(extent.max())
    s = float(h["spacing"])

    water = water_material(h["materials"][0]["absorption"] if h["materials"] else (0.55, 0.16, 0.09),
                           args.absorption_scale)
    spray_mat = principled("Spray", (0.95, 0.97, 1.0), 0.12, **{"Transmission Weight": 0.85, "IOR": 1.333})
    concrete = principled("Concrete", (0.46, 0.45, 0.43), 0.82)
    terrain = principled("Terrain", (0.30, 0.27, 0.22), 0.9)
    paddle = principled("Paddle", (0.82, 0.83, 0.85), 0.35)
    acrylic = principled("Acrylic", (1.0, 1.0, 1.0), 0.04, **{"Transmission Weight": 1.0, "IOR": 1.49})
    frame_mat = principled("Frame", (0.10, 0.105, 0.115), 0.35, **{"Metallic": 1.0})

    # Floor: a large plane with a scale grid, so size reads at a glance.
    grid = 10 ** math.floor(math.log10(size / 4))
    if size / grid > 25:
        grid *= 5
    bpy.ops.mesh.primitive_plane_add(size=size * 30, location=(0, 0, float(dmin[1]) - 1e-4))
    floor = bpy.context.active_object
    floor.name = "Floor"
    floor.data.materials.append(floor_material(grid))

    # Fluid: dense particles become the surface, isolated ones become spray.
    pts = frame["positions"]
    if len(pts):
        _, isolated = fluid_surface("Water", pts, s, water)
        spray(pts[isolated], s, spray_mat)

    # Obstacles at their recorded offset (wave paddles move).
    for o, off in zip(h["obstacles"], frame["offsets"]):
        mat = paddle if o["name"].startswith("paddle") or "paddle" in o["name"] else concrete
        if o["type"] == SHAPE_BOX:
            box_object(o["name"], o["min"] + off, o["max"] + off, mat, bevel=min(0.004, s * 0.5))
        elif o["type"] == SHAPE_SPHERE:
            bpy.ops.mesh.primitive_uv_sphere_add(radius=o["radius"], location=to_b(o["center"] + off), segments=64, ring_count=32)
            bpy.context.active_object.data.materials.append(mat)
            bpy.ops.object.shade_smooth()
        elif o["type"] == SHAPE_CYLINDER:
            rot = {0: (0, math.pi / 2, 0), 1: (0, 0, 0), 2: (math.pi / 2, 0, 0)}[int(o["axis"])]
            bpy.ops.mesh.primitive_cylinder_add(radius=o["radius"], depth=2 * o["half_length"],
                                                location=to_b(o["center"] + off), rotation=rot, vertices=96)
            bpy.context.active_object.data.materials.append(mat)
            bpy.ops.object.shade_smooth()
        elif o["type"] == SHAPE_HEIGHTFIELD:
            heightfield(o, off, terrain)

    # Camera, using the viewer's orbit convention (src/render/Camera.cpp).
    cam = camera_spec or {}
    target = np.asarray(cam.get("target", (dmin + dmax) / 2), dtype=float)
    dist = float(cam.get("distance", size * 1.6)) * args.distance_scale
    yaw, pitch = math.radians(cam.get("yaw_deg", 38.0)), math.radians(cam.get("pitch_deg", 20.0))
    eye = target + dist * np.array([math.cos(pitch) * math.sin(yaw), math.sin(pitch), math.cos(pitch) * math.cos(yaw)])
    cam_data = bpy.data.cameras.new("Camera")
    cam_data.lens_unit = "FOV"
    cam_data.angle = math.radians(cam.get("fov_deg", 40.0))
    cam_data.clip_start, cam_data.clip_end = size * 0.005, size * 100
    cam_ob = bpy.data.objects.new("Camera", cam_data)
    scene.collection.objects.link(cam_ob)
    cam_ob.location = to_b(eye)
    direction = np.array(to_b(target)) - np.array(to_b(eye))
    cam_ob.rotation_euler = _look_rotation(direction)
    scene.camera = cam_ob
    if args.dof:
        cam_data.dof.use_dof = True
        cam_data.dof.focus_distance = float(np.linalg.norm(direction))
        cam_data.dof.aperture_fstop = args.dof

    # Tank: acrylic panels on the solid walls facing away from the camera, a thin frame on the
    # edges, so the fluid is never seen through a pane.
    t = max(s * 0.25, size * 0.004)
    centre = (dmin + dmax) / 2
    for face in range(6):
        axis, side = face // 2, face % 2
        if axis == 1 or h["faces"][face] != 0:
            continue
        lo, hi = dmin.copy(), dmax.copy()
        normal = np.zeros(3)
        normal[axis] = 1.0 if side else -1.0
        if side:
            lo[axis], hi[axis] = dmax[axis], dmax[axis] + t
        else:
            lo[axis], hi[axis] = dmin[axis] - t, dmin[axis]
        if np.dot(normal, eye - centre) < 0:
            box_object(f"Wall{face}", lo, hi, acrylic)
    for a in range(3):
        for c1 in (0, 1):
            for c2 in (0, 1):
                lo, hi = dmin.copy(), dmax.copy()
                o1, o2 = [x for x in range(3) if x != a]
                for o_axis, c in ((o1, c1), (o2, c2)):
                    v = dmax[o_axis] if c else dmin[o_axis]
                    lo[o_axis], hi[o_axis] = v - t / 2, v + t / 2
                if h["faces"][o1 * 2 + c1] != 0 and h["faces"][o2 * 2 + c2] != 0:
                    continue  # an edge between two open faces is not part of the tank
                box_object(f"Edge{a}{c1}{c2}", lo, hi, frame_mat)

    _lights(scene, target, eye, size)
    _render_settings(scene, args)


def _look_rotation(direction):
    from mathutils import Vector
    return Vector(direction).to_track_quat("-Z", "Y").to_euler()


def _lights(scene, target, eye, size):
    from mathutils import Vector
    world = bpy.data.worlds.new("World")
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (0.010, 0.011, 0.013, 1)
    bg.inputs["Strength"].default_value = 1.0
    scene.world = world

    tb = np.array(to_b(target))
    view = np.array(to_b(eye)) - tb
    view_h = view.copy(); view_h[2] = 0
    view_h /= max(np.linalg.norm(view_h), 1e-6)
    side = np.cross(view_h, (0, 0, 1))

    def area(name, position, energy, color, sz):
        data = bpy.data.lights.new(name, "AREA")
        data.energy = energy
        data.color = color
        data.shape = "DISK"
        data.size = sz
        ob = bpy.data.objects.new(name, data)
        scene.collection.objects.link(ob)
        ob.location = tuple(position)
        ob.rotation_euler = Vector(tb - np.asarray(position)).to_track_quat("-Z", "Y").to_euler()
        return ob

    d = size * 1.8
    e = 12.0 * size * size
    area("Key", tb + d * (0.55 * view_h + 0.6 * side) + (0, 0, d * 1.1), e * 3.2, (1.0, 0.96, 0.90), size * 1.4)
    area("Fill", tb + d * (0.7 * view_h - 0.9 * side) + (0, 0, d * 0.35), e * 0.7, (0.86, 0.92, 1.0), size * 2.2)
    area("Rim", tb - d * 1.2 * view_h + (0, 0, d * 0.75), e * 4.0, (0.80, 0.90, 1.0), size * 0.9)
    area("Top", tb + (0, 0, d * 1.6), e * 1.2, (1.0, 1.0, 1.0), size * 2.5)


def _render_settings(scene, args):
    scene.render.engine = "CYCLES"
    c = scene.cycles
    c.device = "CPU"
    c.samples = args.samples
    c.use_adaptive_sampling = True
    c.adaptive_threshold = 0.02
    c.use_denoising = True
    try:
        c.denoiser = "OPENIMAGEDENOISE"
    except TypeError:
        pass
    c.max_bounces, c.transmission_bounces, c.transparent_max_bounces = 16, 16, 8
    c.glossy_bounces, c.diffuse_bounces, c.volume_bounces = 6, 3, 1
    c.caustics_reflective = c.caustics_refractive = False
    c.blur_glossy = 1.0
    scene.render.threads_mode = "AUTO"
    scene.render.resolution_x, scene.render.resolution_y = args.width, args.height
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.view_settings.view_transform = "AgX"
    scene.view_settings.look = "AgX - Medium High Contrast"
    scene.view_settings.exposure = args.exposure
    fmt = "JPEG" if args.out.lower().endswith((".jpg", ".jpeg")) else "PNG"
    scene.render.image_settings.file_format = fmt
    if fmt == "JPEG":
        scene.render.image_settings.quality = 92
    scene.render.filepath = args.out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--snapshot", required=True)
    ap.add_argument("--scenario", help="scenario JSON, for its camera block")
    ap.add_argument("--frame", type=int, default=-1, help="frame index (default: last)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--width", type=int, default=1600)
    ap.add_argument("--height", type=int, default=900)
    ap.add_argument("--samples", type=int, default=128)
    ap.add_argument("--exposure", type=float, default=0.0)
    ap.add_argument("--absorption-scale", type=float, default=6.0)
    ap.add_argument("--distance-scale", type=float, default=1.0)
    ap.add_argument("--dof", type=float, default=0.0, help="f-stop; 0 disables depth of field")
    args = ap.parse_args(argv)

    h, frames = read_snapshot(args.snapshot)
    if not frames:
        raise SystemExit("snapshot has no frames")
    frame = frames[args.frame]
    cam = None
    if args.scenario:
        cam = json.load(open(args.scenario)).get("camera")
    t0 = time.time()
    build_scene(h, frame, cam, args)
    bpy.ops.render.render(write_still=True)
    print(f"rendered t={frame['time']:.3f}s, {len(frame['positions'])} particles -> {args.out} "
          f"in {time.time() - t0:.1f}s", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:])
