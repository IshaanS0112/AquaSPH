"use client";

import { useRouter } from "next/navigation";
import { PageHeader } from "@/components/console/shell";
import { ErrorState, KeyValues, LabelTick, ProgressBar } from "@/components/console/bits";
import { Button } from "@/components/ui/button";
import { Skeleton } from "@/components/ui/skeleton";
import { useStats } from "@/lib/api/hooks";
import { formatDuration, formatNumber } from "@/lib/format";

export default function SettingsPage() {
  const router = useRouter();
  const stats = useStats(24);
  const t = stats.data?.tenant;
  const s = stats.data;
  return (
    <>
      <PageHeader title="Settings" description="Your tenant, its limits, and this session." />
      <div className="flex max-w-3xl flex-col gap-6 px-4 py-6 sm:px-8">
        {stats.error && <ErrorState error={stats.error} onRetry={() => stats.refetch()} />}
        <section className="rounded-lg border border-border bg-card p-4">
          <LabelTick>Usage against limits</LabelTick>
          {!s || !t ? <Skeleton className="mt-3 h-24 w-full" /> : (
            <div className="mt-4 flex flex-col gap-4">
              {[
                ["Concurrent jobs", s.states.running, t.max_concurrent_jobs],
                ["Queued jobs", s.states.queued, t.max_queued_jobs],
              ].map(([label, used, max]) => (
                <div key={label as string} className="flex flex-col gap-2">
                  <div className="flex justify-between text-sm"><span className="text-muted-foreground">{label}</span><span className="font-mono">{used} / {max}</span></div>
                  <ProgressBar value={(used as number) / (max as number)} label={label as string} />
                </div>
              ))}
            </div>
          )}
        </section>
        <section className="rounded-lg border border-border bg-card p-4">
          <LabelTick>Tenant</LabelTick>
          {t ? <KeyValues className="mt-3" items={[
            ["Name", t.name],
            ["Particle ceiling per job", formatNumber(t.max_particles)],
            ["Wall-time limit per job", formatDuration(t.max_wall_seconds)],
            ["Rate limit", `${t.rate_per_minute}/min, burst ${t.rate_burst}`],
          ]} /> : <Skeleton className="mt-3 h-24 w-full" />}
          <p className="mt-4 text-xs text-muted-foreground">Limits are set by an operator with <code className="font-mono">aquasph-admin tenant limits</code>.</p>
        </section>
        <section className="rounded-lg border border-destructive/30 bg-card p-4">
          <LabelTick>Session</LabelTick>
          <p className="mt-3 text-sm text-muted-foreground">Your API key is sealed in an HttpOnly cookie for 8 hours and never reaches this page&apos;s JavaScript.</p>
          <Button variant="outline" className="mt-4 border-destructive/40 text-destructive" onClick={async () => { await fetch("/api/session", { method: "DELETE" }); router.push("/login"); }}>Sign out</Button>
        </section>
      </div>
    </>
  );
}
