import { cookies } from "next/headers";
import type { NextRequest } from "next/server";
import { SESSION_COOKIE, apiBaseUrl, isSameOrigin, unseal } from "@/lib/server/session";

// Backend-for-frontend: the browser calls /api/v1/..., this handler adds the caller's API key
// from the sealed session and forwards to the Go API, streaming the response back unchanged.
// That covers JSON, binary snapshots (with Range) and the SSE progress stream.

const REQUEST_HEADERS = ["accept", "content-type", "idempotency-key", "if-none-match", "range", "last-event-id"];
const RESPONSE_HEADERS = [
  "content-type",
  "content-length",
  "content-range",
  "accept-ranges",
  "cache-control",
  "etag",
  "last-modified",
  "location",
  "retry-after",
  "allow",
  "idempotent-replayed",
  "ratelimit-limit",
  "ratelimit-remaining",
  "ratelimit-reset",
  "x-request-id",
  "x-accel-buffering",
];

function problem(status: number, detail: string) {
  return Response.json({ status, detail }, { status, headers: { "content-type": "application/problem+json" } });
}

async function forward(request: NextRequest, ctx: { params: Promise<{ path: string[] }> }) {
  const session = unseal((await cookies()).get(SESSION_COOKIE)?.value);
  if (!session) return problem(401, "not signed in");
  const method = request.method.toUpperCase();
  if (method !== "GET" && method !== "HEAD" && !isSameOrigin(request)) {
    return problem(403, "cross-site request refused");
  }

  const { path } = await ctx.params;
  if (path.some((seg) => seg === "" || seg === "." || seg === "..")) return problem(400, "bad path");
  const url = new URL(`${apiBaseUrl()}/v1/${path.map(encodeURIComponent).join("/")}`);
  url.search = request.nextUrl.search;

  const headers = new Headers();
  for (const name of REQUEST_HEADERS) {
    const v = request.headers.get(name);
    if (v) headers.set(name, v);
  }
  headers.set("authorization", `Bearer ${session.key}`);
  headers.set("x-request-id", request.headers.get("x-request-id") ?? crypto.randomUUID());

  const hasBody = method !== "GET" && method !== "HEAD" && request.body !== null;
  let upstream: Response;
  try {
    upstream = await fetch(url, {
      method,
      headers,
      body: hasBody ? request.body : undefined,
      // @ts-expect-error -- Node's fetch needs duplex for a streamed request body
      duplex: hasBody ? "half" : undefined,
      redirect: "manual",
      cache: "no-store",
      signal: request.signal, // closing the tab or EventSource cancels the upstream request
    });
  } catch {
    if (request.signal.aborted) return new Response(null, { status: 499 });
    return problem(502, "the AquaSPH API is unreachable");
  }

  const out = new Headers();
  for (const name of RESPONSE_HEADERS) {
    const v = upstream.headers.get(name);
    if (v) out.set(name, v);
  }
  if (out.get("content-type")?.startsWith("text/event-stream")) {
    out.set("cache-control", "no-cache, no-transform");
    out.set("x-accel-buffering", "no");
  }
  return new Response(upstream.body, { status: upstream.status, statusText: upstream.statusText, headers: out });
}

export { forward as GET, forward as POST, forward as PUT, forward as PATCH, forward as DELETE };
