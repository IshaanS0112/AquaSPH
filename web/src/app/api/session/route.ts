import { cookies } from "next/headers";
import { NextResponse, type NextRequest } from "next/server";
import {
  API_KEY_PATTERN,
  SESSION_COOKIE,
  SESSION_MAX_AGE_S,
  apiBaseUrl,
  cookieOptions,
  isSameOrigin,
  seal,
  unseal,
} from "@/lib/server/session";

function problem(status: number, detail: string) {
  return NextResponse.json({ status, detail }, { status, headers: { "content-type": "application/problem+json" } });
}

function secureCookies(request: NextRequest) {
  return request.nextUrl.protocol === "https:" || process.env.NODE_ENV === "production";
}

// Sign in: check the key against the API, then keep it sealed in an HttpOnly cookie.
export async function POST(request: NextRequest) {
  if (!isSameOrigin(request)) return problem(403, "cross-site request refused");
  let apiKey = "";
  try {
    const body = (await request.json()) as { api_key?: unknown };
    apiKey = typeof body.api_key === "string" ? body.api_key.trim() : "";
  } catch {
    return problem(400, "send JSON: {\"api_key\": \"aqk_...\"}");
  }
  if (!API_KEY_PATTERN.test(apiKey)) {
    return problem(422, "that is not an AquaSPH API key; keys look like aqk_xxxxxxxx_…");
  }

  let res: Response;
  try {
    res = await fetch(`${apiBaseUrl()}/v1/stats?hours=1`, {
      headers: { authorization: `Bearer ${apiKey}` },
      cache: "no-store",
      signal: AbortSignal.timeout(10_000),
    });
  } catch {
    return problem(502, "the AquaSPH API is unreachable; is it running?");
  }
  if (res.status === 401) return problem(401, "the API rejected this key: it is unknown or revoked");
  if (!res.ok) return problem(502, `the API answered ${res.status}`);
  const stats = (await res.json()) as { tenant?: { name?: string } };

  const prefix = apiKey.slice(4, 12);
  const tenant = stats.tenant?.name ?? "";
  const token = seal({ key: apiKey, prefix, tenant, exp: Math.floor(Date.now() / 1000) + SESSION_MAX_AGE_S });
  (await cookies()).set(SESSION_COOKIE, token, cookieOptions(secureCookies(request)));
  return NextResponse.json({ tenant, key_prefix: prefix });
}

export async function GET() {
  const s = unseal((await cookies()).get(SESSION_COOKIE)?.value);
  if (!s) return NextResponse.json({ authenticated: false }, { status: 401 });
  return NextResponse.json({
    authenticated: true,
    tenant: s.tenant,
    key_prefix: s.prefix,
    expires_at: new Date(s.exp * 1000).toISOString(),
  });
}

export async function DELETE(request: NextRequest) {
  if (!isSameOrigin(request)) return problem(403, "cross-site request refused");
  (await cookies()).delete(SESSION_COOKIE);
  return new NextResponse(null, { status: 204 });
}
