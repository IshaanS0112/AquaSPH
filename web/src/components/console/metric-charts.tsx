"use client";

import { CartesianGrid, Line, LineChart, XAxis, YAxis } from "recharts";
import { ChartContainer, ChartTooltip, ChartTooltipContent, type ChartConfig } from "@/components/ui/chart";
import { LabelTick } from "@/components/console/bits";
import type { Metrics } from "@/lib/api/types";
import { formatPercent, formatSig } from "@/lib/format";

const axis = { fill: "var(--text-subtle)", fontSize: 11, fontFamily: "var(--font-mono)" };

function Panel({ title, claim, children }: { title: string; claim: string; children: React.ReactNode }) {
  return (
    <div className="rounded-lg border border-border bg-card p-4">
      <LabelTick>{title}</LabelTick>
      <h3 className="mt-2 mb-3 text-sm font-medium text-foreground">{claim}</h3>
      {children}
    </div>
  );
}

function Chart({ data, config, keys }: { data: Record<string, number>[]; config: ChartConfig; keys: string[] }) {
  return (
    <ChartContainer config={config} className="aspect-auto h-52 w-full">
      <LineChart data={data} margin={{ left: -8, right: 8, top: 4 }}>
        <CartesianGrid vertical={false} stroke="var(--border)" />
        <XAxis dataKey="t" tickLine={false} axisLine={false} tick={axis} tickFormatter={(v) => `${(+v).toFixed(2)}s`} minTickGap={32} />
        <YAxis tickLine={false} axisLine={false} tick={axis} width={52} tickFormatter={(v) => formatSig(+v, 3)} />
        <ChartTooltip content={<ChartTooltipContent indicator="line" labelFormatter={(_, p) => `t = ${(+p?.[0]?.payload?.t).toFixed(3)} s`} />} />
        {keys.map((k) => (
          <Line key={k} dataKey={k} type="monotone" dot={false} strokeWidth={k === keys[0] ? 2 : 1.5} stroke={`var(--color-${k})`} isAnimationActive={false} />
        ))}
      </LineChart>
    </ChartContainer>
  );
}

export function MetricCharts({ metrics }: { metrics: Metrics }) {
  const tl = metrics.timeline;
  if (!tl || tl.t.length < 2) return null;
  const rows = tl.t.map((t, i) => ({
    t,
    total: tl.kinetic_energy[i] + tl.potential_energy[i],
    kinetic: tl.kinetic_energy[i],
    potential: tl.potential_energy[i],
    max_speed: tl.max_speed[i],
    near_rest: tl.near_rest[i] * 100,
    volume: tl.volume[i],
  }));
  const e0 = rows[0].total;
  const e1 = rows.at(-1)!.total;
  const lost = e0 > 0 ? (e0 - e1) / e0 : 0;
  const vPeak = Math.max(...tl.max_speed);
  const vol0 = tl.volume[0];
  const volDrift = vol0 > 0 ? (tl.volume.at(-1)! - vol0) / vol0 : 0;

  return (
    <div className="grid grid-cols-1 gap-4 lg:grid-cols-2">
      <Panel title="Energy" claim={`Viscosity and impacts dissipated ${formatPercent(lost, 0)} of the mechanical energy`}>
        <Chart data={rows} keys={["total", "kinetic", "potential"]} config={{ total: { label: "Total (J)", color: "var(--chart-1)" }, kinetic: { label: "Kinetic", color: "var(--chart-2)" }, potential: { label: "Potential", color: "var(--chart-6)" } }} />
      </Panel>
      <Panel title="Dynamics" claim={`Peak particle speed ${formatSig(vPeak, 3)} m/s`}>
        <Chart data={rows} keys={["max_speed"]} config={{ max_speed: { label: "Max speed (m/s)", color: "var(--chart-1)" } }} />
      </Panel>
      <Panel title="Compressibility" claim={`${formatPercent(tl.near_rest.at(-1), 0)} of particles end within 1% of rest density`}>
        <Chart data={rows} keys={["near_rest"]} config={{ near_rest: { label: "Within 1% of rest (%)", color: "var(--chart-1)" } }} />
      </Panel>
      <Panel title="Conservation" claim={`Fluid volume drifted ${volDrift >= 0 ? "+" : "−"}${formatPercent(Math.abs(volDrift), 1)} over the run`}>
        <Chart data={rows} keys={["volume"]} config={{ volume: { label: "Volume (m³)", color: "var(--chart-1)" } }} />
      </Panel>
    </div>
  );
}
