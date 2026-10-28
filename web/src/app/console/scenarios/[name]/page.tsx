"use client";

import Image from "next/image";
import { use, useState } from "react";
import { useRouter } from "next/navigation";
import { Loader2, Play } from "lucide-react";
import { toast } from "sonner";
import { PageHeader } from "@/components/console/shell";
import { ErrorState, KeyValues, LabelTick, TierBadge } from "@/components/console/bits";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { Textarea } from "@/components/ui/textarea";
import { useScenario, useSubmitJob } from "@/lib/api/hooks";
import { ApiError } from "@/lib/api/client";
import { catalogByName } from "@/lib/catalog";
import { scenarioLabel } from "@/lib/format";

// "/materials/0/viscosity = 1.0" lines become the API's JSON-pointer overrides.
function parseOverrides(text: string): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const raw of text.split("\n")) {
    const line = raw.trim();
    if (!line) continue;
    const i = line.indexOf("=");
    if (i < 0) throw new Error(`"${line}" is not pointer = value`);
    const key = line.slice(0, i).trim();
    const v = line.slice(i + 1).trim();
    try {
      out[key] = JSON.parse(v);
    } catch {
      out[key] = v;
    }
  }
  return out;
}

export default function ScenarioPage({ params }: { params: Promise<{ name: string }> }) {
  const { name } = use(params);
  const router = useRouter();
  const entry = catalogByName.get(name);
  const scenario = useScenario(name);
  const submit = useSubmitJob();
  const [quality, setQuality] = useState("low");
  const [simTime, setSimTime] = useState("");
  const [overrides, setOverrides] = useState("");
  const [errors, setErrors] = useState<Record<string, string>>({});

  async function run(e: React.FormEvent) {
    e.preventDefault();
    setErrors({});
    let ov: Record<string, unknown>;
    try {
      ov = parseOverrides(overrides);
    } catch (err) {
      setErrors({ overrides: (err as Error).message });
      return;
    }
    try {
      const job = await submit.mutateAsync({
        scenario: name,
        quality,
        sim_time: simTime ? Number(simTime) : undefined,
        overrides: Object.keys(ov).length ? ov : undefined,
      });
      toast.success(job.cache_hit ? "Served from the result cache" : "Job queued");
      router.push(`/console/jobs/${job.id}`);
    } catch (err) {
      if (err instanceof ApiError) setErrors({ form: err.message, ...err.fieldErrors() });
      else setErrors({ form: "Submission failed" });
    }
  }

  const spec = scenario.data?.spec;
  return (
    <>
      <PageHeader
        eyebrow={<LabelTick>Scenario</LabelTick>}
        title={<span className="capitalize">{scenarioLabel(name)}</span>}
        description={entry?.description ?? scenario.data?.description}
        action={entry && <TierBadge tier={entry.tier} />}
      />
      <div className="grid grid-cols-1 gap-6 px-4 py-6 sm:px-8 xl:grid-cols-12">
        <div className="flex flex-col gap-6 xl:col-span-8">
          {entry?.render && (
            <figure className="overflow-hidden rounded-lg border border-border bg-card">
              <div className="relative aspect-video">
                <Image src={entry.render.src} alt={`${name} rendered in Blender`} fill priority sizes="(min-width:1280px) 60vw, 100vw" className="object-cover" />
              </div>
              <figcaption className="px-4 py-3 font-mono text-2xs text-muted-foreground">
                Blender Cycles render of the solver state at t = {entry.render.time} s ({entry.render.quality} quality)
              </figcaption>
            </figure>
          )}
          {(entry?.approximation || scenario.data?.approximation) && (
            <div className="rounded-lg border border-border bg-card p-4">
              <LabelTick>Where the model is approximate</LabelTick>
              <p className="mt-2 text-sm text-muted-foreground">{entry?.approximation || scenario.data?.approximation}</p>
            </div>
          )}
          {scenario.error && <ErrorState error={scenario.error} onRetry={() => scenario.refetch()} />}
          {spec && (
            <div className="rounded-lg border border-border bg-card p-4">
              <LabelTick>Specification</LabelTick>
              <pre className="mt-3 max-h-96 overflow-auto rounded-md bg-surface-2 p-3 font-mono text-xs text-muted-foreground">
                {JSON.stringify(spec, null, 2)}
              </pre>
            </div>
          )}
        </div>

        <form onSubmit={run} className="flex h-fit flex-col gap-4 rounded-lg border border-border bg-card p-4 xl:col-span-4">
          <LabelTick>Run</LabelTick>
          <div className="flex flex-col gap-2">
            <Label htmlFor="quality">Quality</Label>
            <Select value={quality} onValueChange={setQuality}>
              <SelectTrigger id="quality" className="w-full">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="low">Low · seconds to minutes</SelectItem>
                <SelectItem value="medium">Medium · ~6× the particles</SelectItem>
                <SelectItem value="high">High · ~36× the particles</SelectItem>
              </SelectContent>
            </Select>
          </div>
          <div className="flex flex-col gap-2">
            <Label htmlFor="sim-time">Simulated time (s)</Label>
            <Input
              id="sim-time"
              inputMode="decimal"
              placeholder={entry?.simulatedTime ? `${entry.simulatedTime} (scenario default)` : "scenario default"}
              value={simTime}
              onChange={(e) => setSimTime(e.target.value)}
              className="font-mono"
            />
            {errors.sim_time && <p className="text-sm text-destructive">{errors.sim_time}</p>}
          </div>
          <div className="flex flex-col gap-2">
            <Label htmlFor="overrides">Overrides</Label>
            <Textarea
              id="overrides"
              rows={3}
              placeholder="/materials/0/viscosity = 1.0"
              value={overrides}
              onChange={(e) => setOverrides(e.target.value)}
              className="font-mono text-xs"
            />
            <p className="text-xs text-muted-foreground">One JSON pointer per line. Identical requests come back from the cache.</p>
            {Object.entries(errors)
              .filter(([k]) => k.startsWith("overrides") || k.startsWith("/"))
              .map(([k, v]) => (
                <p key={k} className="text-sm text-destructive">
                  {v}
                </p>
              ))}
          </div>
          {errors.form && <p role="alert" className="text-sm text-destructive">{errors.form}</p>}
          <Button type="submit" disabled={submit.isPending}>
            {submit.isPending ? <Loader2 size={16} className="animate-spin" /> : <Play size={16} strokeWidth={1.5} />}
            Run simulation
          </Button>
          {spec?.domain && (
            <KeyValues
              className="mt-2 border-t border-border pt-4"
              items={[
                ["Domain", `${entry?.domain.join(" × ")} m`],
                ["Duration", `${entry?.simulatedTime ?? "—"} s`],
                ["Materials", entry?.materials.join(", ") ?? "—"],
              ]}
            />
          )}
        </form>
      </div>
    </>
  );
}
