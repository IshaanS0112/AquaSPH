// Reader for the solver's particle snapshot format (docs/snapshots.md). Mirrors
// src/io/Snapshot.cpp. Positions stay quantised (Uint16Array views into the file, no copy) and
// are decoded on the GPU, so a 100k-particle frame costs nothing to "parse".

export const SHAPE = { box: 0, sphere: 1, cylinder: 2, heightfield: 3 } as const;

export type SnapshotObstacle = {
  name: string;
  type: number;
  axis: number;
  min: [number, number, number];
  max: [number, number, number];
  center: [number, number, number];
  radius: number;
  halfLength: number;
  heightfield?: {
    base: number;
    origin: [number, number];
    slope: [number, number];
    minHeight: number;
    maxHeight: number;
    bumps: [number, number, number, number][]; // amplitude, centre x, centre z, sigma
  };
};

export type SnapshotFrame = {
  time: number;
  count: number;
  obstacleOffsets: Float32Array; // 3 per obstacle
  positions: Uint16Array; // 3 per particle, quantised against the header's box
  speed: Uint8Array; // 0..255 of the reference speed
};

export type Snapshot = {
  frameCount: number;
  truncated: boolean;
  domainMin: [number, number, number];
  domainMax: [number, number, number];
  quantMin: [number, number, number];
  quantMax: [number, number, number];
  particleRadius: number;
  referenceSpeed: number;
  spacing: number;
  smoothingRadius: number;
  faces: number[]; // 0 solid, 1 open: -x +x -y +y -z +z
  materials: { name: string; absorption: [number, number, number]; restDensity: number }[];
  obstacles: SnapshotObstacle[];
  frames: SnapshotFrame[];
};

const MAGIC = "AQSNAP1\n";
const FRAME_MAGIC = 0x4d415246;
const UNKNOWN_COUNT = 0xffffffff;

export class SnapshotError extends Error {}

export function parseSnapshot(buffer: ArrayBuffer): Snapshot {
  const dv = new DataView(buffer);
  const bytes = new Uint8Array(buffer);
  if (buffer.byteLength < 100 || new TextDecoder().decode(bytes.subarray(0, 8)) !== MAGIC) {
    throw new SnapshotError("not an AquaSPH snapshot");
  }
  const version = dv.getUint32(8, true);
  if (version !== 1) throw new SnapshotError(`unsupported snapshot version ${version}`);
  const headerSize = dv.getUint32(12, true);
  const frameCount = dv.getUint32(16, true);
  const flags = dv.getUint32(20, true);
  const f32 = (o: number) => dv.getFloat32(o, true);
  const vec3 = (o: number): [number, number, number] => [f32(o), f32(o + 4), f32(o + 8)];

  let pos = 100;
  const str = () => {
    const n = bytes[pos];
    const s = new TextDecoder().decode(bytes.subarray(pos + 1, pos + 1 + n));
    pos += 1 + n;
    return s;
  };
  const materials: Snapshot["materials"] = [];
  const materialCount = dv.getUint16(94, true);
  for (let i = 0; i < materialCount; i++) {
    const name = str();
    materials.push({ name, absorption: vec3(pos), restDensity: f32(pos + 12) });
    pos += 16;
  }
  const obstacles: SnapshotObstacle[] = [];
  const obstacleCount = dv.getUint32(96, true);
  for (let i = 0; i < obstacleCount; i++) {
    const type = bytes[pos];
    const axis = bytes[pos + 1];
    pos += 2;
    const name = str();
    const o: SnapshotObstacle = {
      name,
      type,
      axis,
      min: vec3(pos),
      max: vec3(pos + 12),
      center: vec3(pos + 24),
      radius: f32(pos + 36),
      halfLength: f32(pos + 40),
    };
    pos += 44;
    if (type === SHAPE.heightfield) {
      const bumps: [number, number, number, number][] = [];
      const n = dv.getUint32(pos + 28, true);
      for (let b = 0; b < n; b++) {
        const at = pos + 32 + b * 16;
        bumps.push([f32(at), f32(at + 4), f32(at + 8), f32(at + 12)]);
      }
      o.heightfield = {
        base: f32(pos),
        origin: [f32(pos + 4), f32(pos + 8)],
        slope: [f32(pos + 12), f32(pos + 16)],
        minHeight: f32(pos + 20),
        maxHeight: f32(pos + 24),
        bumps,
      };
      pos += 32 + n * 16;
    }
    obstacles.push(o);
  }
  if (pos > headerSize) throw new SnapshotError("corrupt snapshot header");

  const withMaterial = (flags & 1) !== 0;
  const frames: SnapshotFrame[] = [];
  let at = headerSize;
  while (at + 16 <= buffer.byteLength) {
    if (frameCount !== UNKNOWN_COUNT && frames.length >= frameCount) break;
    if (dv.getUint32(at, true) !== FRAME_MAGIC) throw new SnapshotError(`corrupt frame at byte ${at}`);
    const time = f32(at + 4);
    const count = dv.getUint32(at + 8, true);
    const size = (16 + 12 * obstacleCount + 7 * count + (withMaterial ? count : 0) + 3) & ~3;
    if (at + size > buffer.byteLength) break; // a torn final frame: the writer was killed
    const offsetsAt = at + 16;
    const posAt = offsetsAt + 12 * obstacleCount;
    frames.push({
      time,
      count,
      obstacleOffsets: new Float32Array(buffer.slice(offsetsAt, offsetsAt + 12 * obstacleCount)),
      positions: new Uint16Array(buffer, posAt, count * 3),
      speed: new Uint8Array(buffer, posAt + 6 * count, count),
    });
    at += size;
  }

  return {
    frameCount: frames.length,
    truncated: (flags & 2) !== 0,
    domainMin: vec3(24),
    domainMax: vec3(36),
    quantMin: vec3(48),
    quantMax: vec3(60),
    particleRadius: f32(72),
    referenceSpeed: f32(76),
    spacing: f32(80),
    smoothingRadius: f32(84),
    faces: Array.from(bytes.subarray(88, 94)),
    materials,
    obstacles,
    frames,
  };
}

// Decodes one particle's position, for tests and picking (rendering decodes in the shader).
export function decodePosition(s: Snapshot, frame: SnapshotFrame, i: number): [number, number, number] {
  const q = frame.positions;
  return [0, 1, 2].map((a) => s.quantMin[a] + ((s.quantMax[a] - s.quantMin[a]) * q[3 * i + a]) / 65535) as [
    number,
    number,
    number,
  ];
}

export function heightAt(o: SnapshotObstacle, x: number, z: number): number {
  const hf = o.heightfield;
  if (!hf) return o.max[1];
  let y = hf.base + hf.slope[0] * (x - hf.origin[0]) + hf.slope[1] * (z - hf.origin[1]);
  for (const [a, cx, cz, s] of hf.bumps) {
    if (s <= 0) continue;
    const dx = x - cx;
    const dz = z - cz;
    y += a * Math.exp(-(dx * dx + dz * dz) / (2 * s * s));
  }
  return Math.min(Math.max(y, hf.minHeight), hf.maxHeight);
}
