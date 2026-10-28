"use client";

import { useRouter } from "next/navigation";
import { useState } from "react";
import { Loader2, Plus, Trash2 } from "lucide-react";
import { toast } from "sonner";
import { PageHeader } from "@/components/console/shell";
import { LabelTick } from "@/components/console/bits";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { useCreateSweep } from "@/lib/api/hooks";
import { ApiError } from "@/lib/api/client";
import { catalog } from "@/lib/catalog";
import { scenarioLabel } from "@/lib/format";

type Axis = { pointer: string; values: string };

export default function NewSweepPage() {
  const router = useRouter();
  const create = useCreateSweep();
  const [scenario, setScenario] = useState("dam_break");
  const [quality, setQuality] = useState("low");
  const [simTime, setSimTime] = useState("0.15");
  const [axes, setAxes] = useState<Axis[]>([{ pointer: "/materials/0/viscosity", values: "0.5, 1, 5" }]);
  const [error, setError] = useState<string | null>(null);

  const grid = Object.fromEntries(
    axes.filter((a) => a.pointer.trim()).map((a) => [a.pointer.trim(), a.values.split(",").map((v) => v.trim()).filter(Boolean).map((v) => (isNaN(+v) ? v : +v))]),
  );
  const total = Object.values(grid).reduce((n, vs) => n * Math.max(vs.length, 1), 1);

  async function submit(e: React.FormEvent) {
    e.preventDefault();
    setError(null);
    try {
      const sw = await create.mutateAsync({ scenario, quality, sim_time: simTime ? +simTime : undefined, grid });
      toast.success(`Sweep of ${sw.total} jobs queued`);
      router.push(`/console/sweeps/${sw.id}`);
    } catch (err) {
      setError(err instanceof ApiError ? Object.values(err.fieldErrors()).join("; ") || err.message : "Could not create the sweep");
    }
  }

  return (
    <>
      <PageHeader title="New sweep" description="Every combination of the values below becomes one job. Combinations already computed come back from the cache." />
      <form onSubmit={submit} className="flex max-w-3xl flex-col gap-6 px-4 py-6 sm:px-8">
        <div className="grid grid-cols-1 gap-4 sm:grid-cols-3">
          <div className="flex flex-col gap-2">
            <Label>Scenario</Label>
            <Select value={scenario} onValueChange={setScenario}><SelectTrigger className="w-full"><SelectValue /></SelectTrigger>
              <SelectContent>{catalog.map((s) => <SelectItem key={s.name} value={s.name} className="capitalize">{scenarioLabel(s.name)}</SelectItem>)}</SelectContent></Select>
          </div>
          <div className="flex flex-col gap-2">
            <Label>Quality</Label>
            <Select value={quality} onValueChange={setQuality}><SelectTrigger className="w-full"><SelectValue /></SelectTrigger>
              <SelectContent><SelectItem value="low">low</SelectItem><SelectItem value="medium">medium</SelectItem><SelectItem value="high">high</SelectItem></SelectContent></Select>
          </div>
          <div className="flex flex-col gap-2">
            <Label htmlFor="t">Simulated time (s)</Label>
            <Input id="t" value={simTime} onChange={(e) => setSimTime(e.target.value)} className="font-mono" />
          </div>
        </div>
        <div className="flex flex-col gap-3 rounded-lg border border-border bg-card p-4">
          <LabelTick>Grid</LabelTick>
          {axes.map((a, i) => (
            <div key={i} className="grid grid-cols-[1fr_1fr_auto] gap-2">
              <Input aria-label="JSON pointer" placeholder="/numerics/xsph_epsilon" value={a.pointer} className="font-mono text-xs"
                onChange={(e) => setAxes((xs) => xs.map((x, k) => (k === i ? { ...x, pointer: e.target.value } : x)))} />
              <Input aria-label="Values" placeholder="0, 0.25, 0.5" value={a.values} className="font-mono text-xs"
                onChange={(e) => setAxes((xs) => xs.map((x, k) => (k === i ? { ...x, values: e.target.value } : x)))} />
              <Button type="button" variant="ghost" size="icon" aria-label="Remove axis" onClick={() => setAxes((xs) => xs.filter((_, k) => k !== i))}><Trash2 size={16} strokeWidth={1.5} /></Button>
            </div>
          ))}
          <Button type="button" variant="secondary" size="sm" className="w-fit" onClick={() => setAxes((xs) => [...xs, { pointer: "", values: "" }])}><Plus size={14} />Add axis</Button>
        </div>
        {error && <p role="alert" className="text-sm text-destructive">{error}</p>}
        <div className="flex items-center gap-4">
          <Button type="submit" disabled={create.isPending || total === 0}>{create.isPending && <Loader2 size={16} className="animate-spin" />}Run {total} jobs</Button>
        </div>
      </form>
    </>
  );
}
