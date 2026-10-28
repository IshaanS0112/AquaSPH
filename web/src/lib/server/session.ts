import "server-only";
import { createCipheriv, createDecipheriv, createHash, randomBytes } from "node:crypto";

// The console never gives the API key to the browser. It lives in an HttpOnly cookie, sealed
// with AES-256-GCM, and route handlers under /api/v1 unseal it and call the Go API with it.
// Script on the page (including injected script) cannot read the key.

export const SESSION_COOKIE = "aq_session";
export const SESSION_MAX_AGE_S = 8 * 60 * 60;

export type Session = {
  key: string; // the API key, aqk_<prefix>_<secret>
  prefix: string;
  tenant: string;
  exp: number; // unix seconds
};

export const API_KEY_PATTERN = /^aqk_[a-z0-9]{8}_[A-Za-z0-9]{32}$/;

let devWarned = false;

function sealingKey(): Buffer {
  const secret = process.env.AQUASPH_SESSION_SECRET;
  if (secret && secret.length >= 32) return createHash("sha256").update(secret).digest();
  if (process.env.NODE_ENV === "production") {
    throw new Error("AQUASPH_SESSION_SECRET must be set to at least 32 characters");
  }
  if (!devWarned) {
    devWarned = true;
    console.warn("[aquasph-web] AQUASPH_SESSION_SECRET is unset; using a development-only key");
  }
  return createHash("sha256").update("aquasph-development-session-key").digest();
}

export function seal(session: Session): string {
  const iv = randomBytes(12);
  const cipher = createCipheriv("aes-256-gcm", sealingKey(), iv);
  const body = Buffer.concat([cipher.update(JSON.stringify(session), "utf8"), cipher.final()]);
  return [iv, body, cipher.getAuthTag()].map((b) => b.toString("base64url")).join(".");
}

// Returns null for anything that is not an intact, unexpired seal: tampering fails the GCM tag.
export function unseal(token: string | undefined, nowMs = Date.now()): Session | null {
  if (!token) return null;
  const parts = token.split(".");
  if (parts.length !== 3) return null;
  try {
    const [iv, body, tag] = parts.map((p) => Buffer.from(p, "base64url"));
    if (iv.length !== 12 || tag.length !== 16) return null;
    const decipher = createDecipheriv("aes-256-gcm", sealingKey(), iv);
    decipher.setAuthTag(tag);
    const plain = Buffer.concat([decipher.update(body), decipher.final()]).toString("utf8");
    const s = JSON.parse(plain) as Partial<Session>;
    if (typeof s.key !== "string" || typeof s.exp !== "number" || s.exp * 1000 <= nowMs) return null;
    return { key: s.key, prefix: s.prefix ?? "", tenant: s.tenant ?? "", exp: s.exp };
  } catch {
    return null;
  }
}

export function cookieOptions(secure: boolean) {
  return {
    httpOnly: true,
    secure,
    sameSite: "lax" as const,
    path: "/",
    maxAge: SESSION_MAX_AGE_S,
  };
}

// State-changing requests must come from this origin. SameSite=Lax already keeps the cookie off
// cross-site POSTs; this check is the second, explicit layer.
export function isSameOrigin(request: Request): boolean {
  const site = request.headers.get("sec-fetch-site");
  if (site) return site === "same-origin";
  const origin = request.headers.get("origin");
  if (!origin) return false;
  const host = request.headers.get("x-forwarded-host") ?? request.headers.get("host");
  try {
    return new URL(origin).host === host;
  } catch {
    return false;
  }
}

export function apiBaseUrl(): string {
  return (process.env.AQUASPH_API_URL ?? "http://localhost:8080").replace(/\/+$/, "");
}
