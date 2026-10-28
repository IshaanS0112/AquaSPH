import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { SnapshotError, decodePosition, parseSnapshot } from "./snapshot";

// Written by the real solver: --scenario dam_break --quality low --time 0.02 --snapshot-frames 2.
const bytes = readFileSync(new URL("../test/fixtures/dam_break.aqsnap", import.meta.url));
const buffer = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);

describe("parseSnapshot", () => {
  const s = parseSnapshot(buffer);

  it("reads the header the solver wrote", () => {
    expect(s.domainMin).toEqual([0, 0, 0]);
    expect(s.domainMax[0]).toBeCloseTo(1.6);
    expect(s.faces[3]).toBe(1); // dam_break's open top
    expect(s.materials[0].name).toBe("water");
    expect(s.truncated).toBe(false);
  });

  it("reads every frame, with positions inside the quantisation box", () => {
    expect(s.frames.length).toBe(3); // t = 0, one interval, and the final state
    expect(s.frames[0].time).toBe(0);
    expect(s.frames[0].count).toBe(3864);
    const [x, y, z] = decodePosition(s, s.frames[0], 0);
    expect(x).toBeGreaterThanOrEqual(0);
    expect(y).toBeGreaterThanOrEqual(0);
    expect(z).toBeLessThanOrEqual(0.3);
  });

  it("drops a torn final frame instead of failing", () => {
    const torn = buffer.slice(0, buffer.byteLength - 9);
    new DataView(torn).setUint32(16, 0xffffffff, true);
    expect(parseSnapshot(torn).frames.length).toBe(2);
  });

  it("rejects other files", () => {
    expect(() => parseSnapshot(new ArrayBuffer(256))).toThrow(SnapshotError);
  });
});
