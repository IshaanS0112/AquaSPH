import { describe, expect, it } from "vitest";
import { isSameOrigin, seal, unseal } from "./session";

const session = { key: "aqk_abcdefgh_" + "A".repeat(32), prefix: "abcdefgh", tenant: "lab", exp: Math.floor(Date.now() / 1000) + 60 };

describe("session sealing", () => {
  it("round-trips", () => expect(unseal(seal(session))).toEqual(session));
  it("never exposes the key in the cookie value", () => expect(seal(session)).not.toContain("aqk_"));
  it("rejects a tampered seal", () => {
    const t = seal(session).split(".");
    t[1] = t[1].slice(0, -2) + (t[1].endsWith("A") ? "BB" : "AA");
    expect(unseal(t.join("."))).toBeNull();
  });
  it("rejects an expired seal", () => expect(unseal(seal({ ...session, exp: 1 }))).toBeNull());
  it("rejects garbage", () => expect(unseal("x.y.z")).toBeNull());
});

describe("isSameOrigin", () => {
  const req = (h: Record<string, string>) => new Request("http://console.test/api/v1/jobs", { method: "POST", headers: h });
  it("trusts Sec-Fetch-Site", () => {
    expect(isSameOrigin(req({ "sec-fetch-site": "same-origin" }))).toBe(true);
    expect(isSameOrigin(req({ "sec-fetch-site": "cross-site" }))).toBe(false);
  });
  it("falls back to Origin against Host", () => {
    expect(isSameOrigin(req({ origin: "http://console.test", host: "console.test" }))).toBe(true);
    expect(isSameOrigin(req({ origin: "https://evil.example", host: "console.test" }))).toBe(false);
    expect(isSameOrigin(req({ host: "console.test" }))).toBe(false);
  });
});
