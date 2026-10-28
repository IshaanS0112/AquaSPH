import { describe, expect, it } from "vitest";
import { formatBytes, formatDuration, formatSig } from "./format";

describe("format", () => {
  it("durations", () => {
    expect(formatDuration(0.015)).toBe("15 ms");
    expect(formatDuration(11.05)).toBe("11.1 s");
    expect(formatDuration(260)).toBe("4m 20s");
    expect(formatDuration(null)).toBe("—");
  });
  it("significant figures", () => {
    expect(formatSig(1105.6, 4)).toBe("1,106");
    expect(formatSig(0.000231)).toBe("2.31e-4");
  });
  it("bytes", () => expect(formatBytes(3_250_000)).toBe("3.1 MB"));
});
