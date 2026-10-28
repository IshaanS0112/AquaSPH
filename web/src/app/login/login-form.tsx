"use client";

import { useRouter, useSearchParams } from "next/navigation";
import { useState } from "react";
import { Loader2 } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";

// Only same-site paths may be a post-login destination: ?next=https://evil.example is ignored.
function safeNext(raw: string | null): string {
  if (!raw || !raw.startsWith("/console") || raw.startsWith("//")) return "/console";
  return raw;
}

export function LoginForm() {
  const router = useRouter();
  const params = useSearchParams();
  const [key, setKey] = useState("");
  const [error, setError] = useState<string | null>(params.get("expired") ? "Your session ended. Sign in again." : null);
  const [pending, setPending] = useState(false);

  async function submit(e: React.FormEvent) {
    e.preventDefault();
    setPending(true);
    setError(null);
    try {
      const res = await fetch("/api/session", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ api_key: key }),
      });
      if (!res.ok) {
        const body = (await res.json().catch(() => ({}))) as { detail?: string };
        setError(body.detail ?? `Sign-in failed (${res.status})`);
        return;
      }
      router.replace(safeNext(params.get("next")));
    } catch {
      setError("The console could not be reached.");
    } finally {
      setPending(false);
    }
  }

  return (
    <form onSubmit={submit} className="flex flex-col gap-4" noValidate>
      <div className="flex flex-col gap-2">
        <Label htmlFor="api-key">API key</Label>
        <Input
          id="api-key"
          name="api-key"
          type="password"
          autoComplete="off"
          spellCheck={false}
          placeholder="aqk_xxxxxxxx_…"
          value={key}
          onChange={(e) => setKey(e.target.value)}
          aria-invalid={error ? true : undefined}
          aria-describedby={error ? "api-key-error" : undefined}
          className="font-mono"
          required
        />
        {error && (
          <p id="api-key-error" role="alert" className="text-sm text-destructive">
            {error}
          </p>
        )}
      </div>
      <Button type="submit" disabled={pending || key.trim().length === 0} className="w-full">
        {pending && <Loader2 size={16} strokeWidth={1.5} className="animate-spin" />}
        {pending ? "Checking key" : "Sign in"}
      </Button>
    </form>
  );
}
