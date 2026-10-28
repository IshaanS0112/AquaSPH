"use client";

import Link from "next/link";
import { use, useMemo } from "react";
import { CartesianGrid, Scatter, ScatterChart, XAxis, YAxis } from "recharts";
import { PageHeader } from "@/components/console/shell";
import { ErrorState, Kpi, LabelTick, StatusBadge } from "@/components/console/bits";
import { ChartContainer, ChartTooltip, ChartTooltipContent } from "@/components/ui/chart";
import { Button } from "@/components/ui/button";
import { Skeleton } from "@/components/ui/skeleton";
import { useCancelSweep, useSweep, useSweepResults } from "@/lib/api/hooks";
import { formatSig, scenarioLabel, shortId } from "@/lib/format";

const METRICS = ["/dynamics/max_speed", "/density/max", "/time/wall_seconds", "/volume/final_m3"];

export default function SweepPage({ params }: { params: Promise<{ id: string }> }) {
  const { id } = use(params);
  const sweep = useSweep(id);
  const s = sweep.data;
  const results = useSweepResults(id, METRICS, s?.state !== "completed");
  const cancel = useCancelSweep(id);
  const axis = results.data?.axes[0];
  const points = useMemo(
    () => (results.data?.rows ?? []).filter((r) => typeof r.params[axis ?? ""] === "number" && typeof r.metrics["/dynamics/max_speed"] === "number")
      .map((r) => ({ x: r.params[axis!] as number, y: r.metrics["/dynamics/max_speed"] as number })),
    [results.data, axis],
  );

  if (sweep.error) return <div className="p-8"><ErrorState error={sweep.error} /></div>;
  if (!s) return <div className="p-8"><Skeleton className="h-40 w-full" /></div>;
  const done = (s.counts.completed ?? 0) + (s.counts.failed ?? 0) + (s.counts.cancelled ?? 0);

  return (
    <>
      <PageHeader
        eyebrow={<LabelTick>Sweep {shortId(s.id)}</LabelTick>}
        title={<span className="capitalize">{scenarioLabel(s.scenario)}</span>}
        description={`${s.quality} quality · ${Object.keys(s.grid).join(" × ")}`}
        action={s.state === "running" && <Button variant="secondary" onClick={() => cancel.mutate()} disabled={cancel.isPending}>Cancel remaining</Button>}
      />
      <div className="flex flex-col gap-6 px-4 py-6 sm:px-8">
        <div className="grid grid-cols-1 gap-4 sm:grid-cols-3">
          <Kpi label="Finished" value={`${done}/${s.total}`} detail={`${s.counts.running ?? 0} running · ${s.counts.queued ?? 0} queued`} />
          <Kpi label="Cache hits" value={s.cache_hits} detail="combinations already computed" />
          <Kpi label="Failed" value={s.counts.failed ?? 0} detail="unstable runs still complete" />
        </div>
        {axis && points.length > 1 && (
          <div className="rounded-lg border border-border bg-card p-4">
            <LabelTick>Response</LabelTick>
            <h2 className="mt-2 mb-3 text-sm font-medium">Peak speed across {axis}</h2>
            <ChartContainer config={{ y: { label: "Max speed (m/s)", color: "var(--chart-1)" } }} className="aspect-auto h-60 w-full">
              <ScatterChart margin={{ left: -8, right: 8 }}>
                <CartesianGrid stroke="var(--border)" />
                <XAxis dataKey="x" type="number" name={axis} tickLine={false} axisLine={false} tick={{ fill: "var(--text-subtle)", fontSize: 11 }} />
                <YAxis dataKey="y" type="number" tickLine={false} axisLine={false} width={48} tick={{ fill: "var(--text-subtle)", fontSize: 11 }} tickFormatter={(v) => formatSig(+v)} />
                <ChartTooltip content={<ChartTooltipContent hideLabel />} />
                <Scatter data={points} fill="var(--color-y)" />
              </ScatterChart>
            </ChartContainer>
          </div>
        )}
        {results.error && <ErrorState error={results.error} />}
        <div className="overflow-x-auto rounded-lg border border-border bg-card">
          <table className="w-full min-w-[720px] text-sm">
            <thead><tr className="h-10 border-b border-border text-left font-mono text-xs tracking-label text-muted-foreground uppercase">
              <th className="px-4 font-normal">Job</th><th className="px-4 font-normal">State</th>
              {results.data?.axes.map((a) => <th key={a} className="px-4 text-right font-normal normal-case">{a}</th>)}
              {METRICS.map((m) => <th key={m} className="px-4 text-right font-normal normal-case">{m}</th>)}
            </tr></thead>
            <tbody>
              {results.data?.rows.map((r) => (
                <tr key={r.job_id} className="h-10 border-b border-border last:border-0 hover:bg-surface-2">
                  <td className="px-4"><Link className="font-mono text-xs text-muted-foreground hover:text-foreground" href={`/console/jobs/${r.job_id}`}>{shortId(r.job_id)}</Link></td>
                  <td className="px-4"><StatusBadge state={r.state} /></td>
                  {results.data!.axes.map((a) => <td key={a} className="px-4 text-right font-mono text-xs">{String(r.params[a])}</td>)}
                  {METRICS.map((m) => <td key={m} className="px-4 text-right font-mono text-xs">{typeof r.metrics[m] === "number" ? formatSig(r.metrics[m] as number, 4) : "—"}</td>)}
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </div>
    </>
  );
}
