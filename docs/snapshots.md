# Particle snapshots

`--snapshots FILE` records the fluid at a fixed simulated-time interval, the way SPlisHSPlasH
exports partio frames at `dataExportFPS`. Two consumers read it: the web console's 3D playback
and the Blender render pipeline (`render/blender/`). Both read only this file, because the header
carries everything needed to rebuild the scene: domain, open faces, materials and obstacle
geometry, including wave paddles and terrain.

```bash
./aquasph --scenario dam_break --quality low --snapshots out.aqsnap --snapshot-interval 0.01
```

| Flag | Default | Meaning |
|---|---|---|
| `--snapshots FILE` | off | Write frames to `FILE` |
| `--snapshot-interval S` | the scenario's `output_interval` | Simulated seconds between frames |
| `--snapshot-max-mb N` | 512 | Stop writing frames past this size and flag the file truncated |

A frame costs 7 bytes per fluid particle: three 16-bit positions and an 8-bit speed. A low-quality
dam break (3,864 particles) is 27 KB per frame, so 120 frames is 3.2 MB. Boundary particles are
not stored, because a reader draws walls and obstacles from the header instead.

## Guarantees

- **Bit-identical at any thread count.** Positions come from a simulation that is itself
  bit-identical, and quantisation is a pure function of them (`test_snapshot.cpp` compares the
  files byte for byte at 1 and 4 threads).
- **Survives cancellation.** On SIGTERM the solver writes a final frame of the state it stopped
  at and patches the frame count (`CliContract.snapshots_survive_cancel`).
- **Survives SIGKILL.** An unpatched frame count reads as "unknown"; readers take every complete
  frame and drop a torn last one.
- **Bounded.** Past the size budget the writer stops, keeps what it has, and sets the truncated flag.

## Layout

Little-endian. All offsets in bytes.

### Header

| Offset | Type | Field |
|---|---|---|
| 0 | `char[8]` | Magic `AQSNAP1\n` |
| 8 | `u32` | Version (1) |
| 12 | `u32` | Header size, including padding (frames start here) |
| 16 | `u32` | Frame count; `0xFFFFFFFF` when the writer never closed the file |
| 20 | `u32` | Flags: bit 0 = per-particle material byte present, bit 1 = truncated by the budget |
| 24 | `f32[3]` ×2 | Domain min, max (m) |
| 48 | `f32[3]` ×2 | Quantisation box min, max (the domain plus a margin of max(4h, 2% of the largest extent)) |
| 72 | `f32` | Particle radius (half the spacing, m) |
| 76 | `f32` | Reference speed for the speed byte (the scenario's `render.reference_speed`, m/s) |
| 80 | `f32` | Particle spacing (m) |
| 84 | `f32` | Smoothing radius h (m) |
| 88 | `u8[6]` | Face mode for −x +x −y +y −z +z (0 solid, 1 open) |
| 94 | `u16` | Material count M |
| 96 | `u32` | Obstacle count K |
| 100 | | M materials, then K obstacles, then zero padding to a multiple of 4 |

Material: `u8` name length, name bytes, `f32[3]` Beer–Lambert absorption (1/m), `f32` rest
density.

Obstacle: `u8` shape type (0 box, 1 sphere, 2 cylinder, 3 heightfield), `u8` cylinder axis,
`u8` name length, name bytes, `f32[3]` min, `f32[3]` max, `f32[3]` centre, `f32` radius,
`f32` half length. A heightfield then adds `f32` base, `f32[2]` origin (x, z), `f32[2]` slope,
`f32` min height, `f32` max height, `u32` bump count and, per bump, `f32` amplitude, centre x,
centre z, sigma. The surface is `y = base + slope·(p − origin) + Σ a·exp(−|p − c|² / 2σ²)`,
clamped to [min height, max height], evaluated over the shape's x–z bounds.

Obstacles are listed in simulation order, wave paddles last.

### Frame

| Field | Type |
|---|---|
| Magic `FRAM` (`0x4D415246`) | `u32` |
| Time (s) | `f32` |
| Particle count N | `u32` |
| Reserved | `u32` |
| Obstacle translation, one per obstacle | `f32[3]` × K |
| Positions | `u16[3]` × N |
| Speed | `u8` × N |
| Material (only when flag bit 0 is set) | `u8` × N |
| Zero padding to a multiple of 4 | |

Positions decode as `min + (max − min) · q / 65535` per axis, against the quantisation box: at
most 12 µm of error in a 1.6 m tank. Speed decodes as `reference_speed · q / 255` and saturates
at the reference speed. Positions start 4-byte aligned, so a reader can view them in place as a
16-bit array.

## Reading it

Python (numpy), as the Blender script does:

```python
import numpy as np, struct
data = open("out.aqsnap", "rb").read()
header_size, = struct.unpack_from("<I", data, 12)
qmin = np.frombuffer(data, "<f4", 3, 48); qmax = np.frombuffer(data, "<f4", 3, 60)
k, = struct.unpack_from("<I", data, 96)
pos = header_size
t, n = struct.unpack_from("<fI", data, pos + 4)
q = np.frombuffer(data, "<u2", n * 3, pos + 16 + 12 * k).reshape(n, 3)
xyz = qmin + (qmax - qmin) * q / 65535.0
```

The TypeScript reader lives in `web/src/lib/snapshot.ts`, and the C++ reader in `src/io/Snapshot.cpp`.
