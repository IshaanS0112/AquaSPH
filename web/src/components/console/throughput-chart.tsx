"use client";

import { Bar, BarChart, CartesianGrid, XAxis, YAxis } from "recharts";
import { ChartContainer, ChartTooltip, ChartTooltipContent, type ChartConfig } from "@/components/ui/chart";
import type { Stats } from "@/lib/api/types";
import { formatPercent } from "@/lib/format";

// Highlight mode: cache hits carry the point (results the platform did not have to compute), so
// they take the accent. Fresh runs are context, failures keep their status colour.
const config = {
  cache_hits: { label: "From cache", color: "var(--chart-1)" },
  completed: { label: "Fresh runs", color: "var(--chart-6)" },
  failed: { label: "Failed", color: "var(--destructive)" },
} satisfies ChartConfig;

export function throughputClaim(stats: Stats): string {
  const w = stats.window;
  const total = w.completed;
  if (total === 0) return `No completed runs in the last ${stats.window_hours} h`;
  if (w.cache_hits === 0) return `${total} run${total === 1 ? "" : "s"} completed in the last ${stats.window_hours} h, none from cache`;
  return `${formatPercent(w.cache_hit_rate, 0)} of ${total} results came from the cache in the last ${stats.window_hours} h`;
}

export function ThroughputChart({ stats }: { stats: Stats }) {
  const data = stats.hourly.map((h) => ({
    hour: new Date(h.hour).toLocaleTimeString("en-GB", { hour: "2-digit", minute: "2-digit" }),
    completed: h.completed,
    cache_hits: h.cache_hits,
    failed: h.failed,
  }));
  return (
    <ChartContainer config={config} className="aspect-auto h-64 w-full">
      <BarChart data={data} margin={{ left: -16, right: 4, top: 8 }}>
        <CartesianGrid vertical={false} stroke="var(--border)" />
        <XAxis
          dataKey="hour"
          tickLine={false}
          axisLine={false}
          interval="preserveStartEnd"
          minTickGap={24}
          tick={{ fill: "var(--text-subtle)", fontSize: 11, fontFamily: "var(--font-mono)" }}
        />
        <YAxis
          allowDecimals={false}
          tickLine={false}
          axisLine={false}
          width={40}
          tick={{ fill: "var(--text-subtle)", fontSize: 11, fontFamily: "var(--font-mono)" }}
        />
        <ChartTooltip cursor={{ fill: "var(--surface-2)" }} content={<ChartTooltipContent indicator="dot" />} />
        <Bar dataKey="completed" stackId="a" fill="var(--color-completed)" radius={[0, 0, 0, 0]} />
        <Bar dataKey="cache_hits" stackId="a" fill="var(--color-cache_hits)" />
        <Bar dataKey="failed" stackId="a" fill="var(--color-failed)" radius={[3, 3, 0, 0]} />
      </BarChart>
    </ChartContainer>
  );
}

export function ChartLegendRow() {
  return (
    <div className="flex flex-wrap items-center gap-4 text-xs text-muted-foreground">
      {Object.entries(config).map(([key, c]) => (
        <span key={key} className="inline-flex items-center gap-2">
          <span className="size-2 rounded-sm" style={{ background: c.color }} aria-hidden="true" />
          {c.label}
        </span>
      ))}
    </div>
  );
}
