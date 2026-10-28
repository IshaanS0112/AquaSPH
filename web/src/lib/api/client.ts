import type { ProblemDetails } from "./types";

// Every API error is RFC 9457 problem+json; ApiError keeps it whole so forms can show field errors.
export class ApiError extends Error {
  readonly status: number;
  readonly problem: ProblemDetails;

  constructor(problem: ProblemDetails) {
    super(problem.detail ?? problem.title ?? `request failed (${problem.status})`);
    this.status = problem.status;
    this.problem = problem;
  }

  fieldErrors(): Record<string, string> {
    const out: Record<string, string> = {};
    for (const e of this.problem.errors ?? []) out[e.field] = out[e.field] ? `${out[e.field]}; ${e.message}` : e.message;
    return out;
  }
}

async function toProblem(res: Response): Promise<ProblemDetails> {
  try {
    const body = (await res.json()) as Partial<ProblemDetails>;
    return { status: res.status, ...body };
  } catch {
    return { status: res.status, detail: res.statusText || `request failed (${res.status})` };
  }
}

function onUnauthorized() {
  if (typeof window !== "undefined" && !window.location.pathname.startsWith("/login")) {
    const next = encodeURIComponent(window.location.pathname + window.location.search);
    // A full navigation on purpose: it drops all cached query data from the old session.
    // eslint-disable-next-line @next/next/no-location-assign-relative-destination
    window.location.href = `/login?next=${next}&expired=1`;
  }
}

type Options = { method?: string; body?: unknown; headers?: Record<string, string>; signal?: AbortSignal };

// api("/jobs") calls the console's /api/v1/jobs, which forwards to the Go API with the session key.
export async function api<T>(path: string, opts: Options = {}): Promise<T> {
  const headers: Record<string, string> = { accept: "application/json", ...opts.headers };
  let body: BodyInit | undefined;
  if (opts.body !== undefined) {
    headers["content-type"] = "application/json";
    body = JSON.stringify(opts.body);
  }
  const res = await fetch(`/api/v1${path}`, { method: opts.method ?? "GET", headers, body, signal: opts.signal });
  if (res.status === 401) onUnauthorized();
  if (!res.ok) throw new ApiError(await toProblem(res));
  if (res.status === 204) return undefined as T;
  return (await res.json()) as T;
}

export async function apiBinary(path: string, signal?: AbortSignal): Promise<ArrayBuffer> {
  const res = await fetch(`/api/v1${path}`, { signal });
  if (res.status === 401) onUnauthorized();
  if (!res.ok) throw new ApiError(await toProblem(res));
  return res.arrayBuffer();
}
