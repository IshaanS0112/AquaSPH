import { apiBaseUrl } from "@/lib/server/session";

// The Go API serves its own OpenAPI document; the console relays it so the link works same-origin.
export async function GET() {
  try {
    const res = await fetch(`${apiBaseUrl()}/openapi.yaml`, { cache: "no-store" });
    return new Response(res.body, { status: res.status, headers: { "content-type": "text/yaml; charset=utf-8" } });
  } catch {
    return new Response("the AquaSPH API is unreachable", { status: 502 });
  }
}
