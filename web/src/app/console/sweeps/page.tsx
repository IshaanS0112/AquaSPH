"use client";

import Link from "next/link";
import { Plus } from "lucide-react";
import { PageHeader } from "@/components/console/shell";
import { EmptyState, ErrorState, ProgressBar, StatusBadge } from "@/components/console/bits";
import { Button } from "@/components/ui/button";
import { Skeleton } from "@/components/ui/skeleton";
import { useSweeps } from "@/lib/api/hooks";
import { formatRelative, scenarioLabel, shortId } from "@/lib/format";

export default function SweepsPage() {
  const sweeps = useSweeps();
  return (
    <>
      <PageHeader
        title="Sweeps"
        description="Parameter studies: one scenario, a grid of overrides, one job per combination."
        action={<Button asChild><Link href="/console/sweeps/new"><Plus size={16} strokeWidth={1.5} />New sweep</Link></Button>}
      />
      <div className="px-4 py-6 sm:px-8">
        {sweeps.error ? <ErrorState error={sweeps.error} onRetry={() => sweeps.refetch()} /> :
          sweeps.isLoading ? <Skeleton className="h-40 w-full" /> :
          sweeps.data?.data.length === 0 ? (
            <EmptyState title="No sweeps yet" body="Vary viscosity, resolution or any JSON pointer across a grid and compare the results." action={<Button asChild><Link href="/console/sweeps/new">Create a sweep</Link></Button>} />
          ) : (
            <div className="overflow-x-auto rounded-lg border border-border bg-card">
              <table className="w-full min-w-[640px] text-sm">
                <thead><tr className="h-10 border-b border-border text-left font-mono text-xs tracking-label text-muted-foreground uppercase">
                  <th className="px-4 font-normal">Sweep</th><th className="px-4 font-normal">Scenario</th><th className="px-4 font-normal">State</th>
                  <th className="px-4 font-normal">Progress</th><th className="px-4 text-right font-normal">Cache hits</th><th className="px-4 text-right font-normal">Created</th>
                </tr></thead>
                <tbody>
                  {sweeps.data?.data.map((s) => {
                    const done = (s.counts.completed ?? 0) + (s.counts.failed ?? 0) + (s.counts.cancelled ?? 0);
                    return (
                      <tr key={s.id} className="h-10 border-b border-border last:border-0 hover:bg-surface-2">
                        <td className="px-4"><Link href={`/console/sweeps/${s.id}`} className="font-mono text-xs text-muted-foreground hover:text-foreground">{shortId(s.id)}</Link></td>
                        <td className="px-4 capitalize">{scenarioLabel(s.scenario)}</td>
                        <td className="px-4"><StatusBadge state={s.state === "running" ? "running_sweep" : "completed"} /></td>
                        <td className="w-48 px-4"><div className="flex items-center gap-2"><ProgressBar value={done / s.total} /><span className="font-mono text-xs text-muted-foreground">{done}/{s.total}</span></div></td>
                        <td className="px-4 text-right font-mono text-xs">{s.cache_hits}</td>
                        <td className="px-4 text-right font-mono text-xs text-muted-foreground">{formatRelative(s.created_at)}</td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          )}
      </div>
    </>
  );
}
