import { NextResponse, type NextRequest } from "next/server";

// An optimistic gate: no session cookie, no console. The real check is in the /api/v1 route
// handlers, which unseal the cookie and let the Go API verify the key on every request.
export function proxy(request: NextRequest) {
  if (!request.cookies.has("aq_session")) {
    const login = new URL("/login", request.url);
    login.searchParams.set("next", request.nextUrl.pathname + request.nextUrl.search);
    return NextResponse.redirect(login);
  }
  return NextResponse.next();
}

export const config = { matcher: ["/console/:path*"] };
