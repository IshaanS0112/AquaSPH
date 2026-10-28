import type { Metadata } from "next";
import { Suspense } from "react";
import { Brand } from "@/components/brand";
import { LoginForm } from "./login-form";

export const metadata: Metadata = { title: "Sign in" };

export default function LoginPage() {
  return (
    <main className="dot-grid flex min-h-dvh items-center justify-center px-4 py-12">
      <div className="w-full max-w-sm">
        <Brand className="mb-8" />
        <div className="rounded-lg border border-border bg-card p-6">
          <h1 className="text-xl font-semibold tracking-display text-foreground">Sign in to the console</h1>
          <p className="mt-1 mb-6 text-sm text-muted-foreground">
            Use a tenant API key. It is checked against the API and kept server-side; this page never stores it.
          </p>
          <Suspense>
            <LoginForm />
          </Suspense>
        </div>
        <p className="mt-6 text-xs text-muted-foreground">
          No key? Run <code className="font-mono text-foreground">make demo-key</code> against your stack, or{" "}
          <code className="font-mono text-foreground">aquasph-admin key create</code>.
        </p>
      </div>
    </main>
  );
}
