"use client";

import dynamic from "next/dynamic";
import Link from "next/link";
import { use } from "react";
import { useQuery } from "@tanstack/react-query";
import { Download, Square, Zap } from "lucide-react";
import { toast } from "sonner";
import { PageHeader } from "@/components/console/shell";
import { EmptyState, ErrorState, KeyValues, LabelTick, ProgressBar, StatusBadge } from "@/components/console/bits";
import { MetricCharts } from "@/components/console/metric-charts";
import { Button } from "@/components/ui/button";
import { Skeleton } from "@/components/ui/skeleton";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { useCancelJob, useJob, useJobArtifacts, useJobEvents, useJobHistory, useJobMetrics } from "@/lib/api/hooks";
import { apiBinary } from "@/lib/api/client";
import { parseSnapshot } from "@/lib/snapshot";
import type { ScenarioSpec } from "@/lib/api/types";
import { durationBetween, formatBytes, formatDuration, formatSig, formatTimestamp, scenarioLabel, shortId } from "@/lib/format";

const ParticleViewer = dynamic(() => import("@/components/viewer/particle-viewer").then((m) => m.ParticleViewer), {
  ssr: false,
  loading: () => <Skeleton className="aspect-video w-full" />,
});

function Playback({ id, hasSnapshot, camera }: { id: string; hasSnapshot: boolean; camera?: ScenarioSpec["camera"] }) {
  const snap = useQuery({
    queryKey: ["snapshot", id],
    queryFn: async ({ signal }) => parseSnapshot(await apiBinary(`/jobs/${id}/artifacts/snapshots.aqsnap`, signal)),
    enabled: hasSnapshot,
    staleTime: Infinity,
  });
  if (!hasSnapshot) return <EmptyState title="No playback for this run" body="Snapshots are recorded by workers with AQUASPH_SNAPSHOT_FRAMES above zero." />;
  if (snap.error) return <ErrorState error={snap.error} onRetry={() => snap.refetch()} />;
  if (!snap.data) return <Skeleton className="aspect-video w-full" />;
  return <ParticleViewer snap={snap.data} camera={camera} />;
}

export default function JobPage({ params }: { params: Promise<{ id: string }> }) {
  const { id } = use(params);
  const job = useJob(id);
  const j = job.data;
  const finished = j && ["completed", "failed", "cancelled"].includes(j.state);
  const live = useJobEvents(id, !!j && !finished);
  const state = live.state ?? j?.state;
  const progress = live.progress ?? j?.progress ?? undefined;
  const hasResult = !!j && (j.state === "completed" || ((j.state === "cancelled" || j.state === "failed") && j.started_at !== null));
  const metrics = useJobMetrics(id, hasResult);
  const artifacts = useJobArtifacts(id, !!finished);
  const history = useJobHistory(id, j?.state);
  const cancel = useCancelJob(id);

  if (job.error) return <div className="p-8"><ErrorState error={job.error} onRetry={() => job.refetch()} /></div>;
  if (!j || !state) return <div className="flex flex-col gap-4 p-8"><Skeleton className="h-10 w-80" /><Skeleton className="aspect-video w-full" /></div>;

  const m = metrics.data?.metrics;
  const runtime = j.cache_hit ? 0 : durationBetween(j.started_at, j.finished_at);
  const hasSnapshot = !!artifacts.data?.some((a) => a.name === "snapshots.aqsnap");

  return (
    <>
      <PageHeader
        eyebrow={<span className="flex items-center gap-2"><LabelTick>Job {shortId(j.id)}</LabelTick><StatusBadge state={state} />{j.cache_hit && <span className="inline-flex items-center gap-1 font-mono text-2xs text-muted-foreground"><Zap size={12} />cache hit</span>}</span>}
        title={<Link href={`/console/scenarios/${j.scenario}`} className="capitalize hover:underline">{scenarioLabel(j.scenario)}</Link>}
        description={`${j.quality} quality · submitted ${formatTimestamp(j.created_at)}`}
        action={
          !finished && (
            <Button variant="secondary" disabled={cancel.isPending || j.cancel_requested} onClick={() => cancel.mutate(undefined, { onSuccess: () => toast("Cancellation requested") })}>
              <Square size={16} strokeWidth={1.5} />
              {j.cancel_requested ? "Cancelling" : "Cancel"}
            </Button>
          )
        }
      />
      <div className="grid grid-cols-1 gap-6 px-4 py-6 sm:px-8 xl:grid-cols-12">
        <div className="flex min-w-0 flex-col gap-6 xl:col-span-8">
          {!finished && (
            <div className="rounded-lg border border-border bg-card p-4">
              <div className="mb-3 flex items-center justify-between">
                <LabelTick>{state === "queued" ? "Waiting for a worker" : "Simulating"}</LabelTick>
                <span className="font-mono text-xs text-muted-foreground">{live.connected ? "live" : "reconnecting"}</span>
              </div>
              <ProgressBar value={progress?.fraction ?? 0} label="Simulation progress" />
              <div className="mt-3 flex flex-wrap gap-x-6 gap-y-1 font-mono text-xs text-muted-foreground tabular">
                <span>t {progress ? `${progress.t.toFixed(3)} / ${progress.t_end} s` : "—"}</span>
                <span>step {progress?.step ?? "—"}</span>
                <span>{progress?.fluid?.toLocaleString() ?? "—"} particles</span>
                <span>|v|max {progress?.max_speed != null ? `${formatSig(progress.max_speed)} m/s` : "—"}</span>
                <span>wall {formatDuration(progress?.wall_s)}</span>
              </div>
            </div>
          )}
          {j.error && <ErrorState error={new Error(`${j.error.code}: ${j.error.message ?? ""}`)} />}
          <Tabs defaultValue="playback">
            <TabsList>
              <TabsTrigger value="playback">3D playback</TabsTrigger>
              <TabsTrigger value="physics">Physics</TabsTrigger>
              <TabsTrigger value="history">History</TabsTrigger>
            </TabsList>
            <TabsContent value="playback" className="mt-4">
              {finished ? <Playback id={id} hasSnapshot={hasSnapshot} camera={j.spec?.camera} /> : <EmptyState title="Playback appears when the run finishes" />}
            </TabsContent>
            <TabsContent value="physics" className="mt-4">
              {metrics.isLoading ? <Skeleton className="h-64 w-full" /> : m ? <MetricCharts metrics={m} /> : <EmptyState title="No metrics yet" body="Charts are drawn from metrics.json once the solver writes it." />}
            </TabsContent>
            <TabsContent value="history" className="mt-4">
              <ol className="flex flex-col rounded-lg border border-border bg-card">
                {history.data?.map((e, i) => (
                  <li key={i} className="flex h-10 items-center gap-4 border-b border-border px-4 last:border-0">
                    <span className="w-40 font-mono text-xs text-muted-foreground">{formatTimestamp(e.at)}</span>
                    <StatusBadge state={e.to_state} />
                    <span className="font-mono text-xs text-muted-foreground">attempt {e.attempt}{e.detail ? ` · ${e.detail}` : ""}</span>
                  </li>
                ))}
              </ol>
            </TabsContent>
          </Tabs>
        </div>

        <aside className="flex flex-col gap-6 xl:col-span-4">
          <div className="rounded-lg border border-border bg-card p-4">
            <LabelTick>Run</LabelTick>
            <KeyValues
              className="mt-3"
              items={[
                ["Outcome", j.outcome ?? "—"],
                ["Runtime", j.cache_hit ? "cache hit" : formatDuration(runtime)],
                ["Attempt", `${j.attempt} of ${j.max_attempts}`],
                ["Particles", m?.particles.fluid_final?.toLocaleString() ?? "—"],
                ["Steps", m?.time.steps.toLocaleString() ?? "—"],
                ["Avg step", m ? `${formatSig(m.time.avg_step_ms)} ms` : "—"],
                ["Density max", m ? `${formatSig(m.density.max, 4)} kg/m³` : "—"],
                ["Spec hash", j.spec_hash.slice(0, 12)],
                ["Solver", j.solver_id?.slice(0, 12) ?? "—"],
              ]}
            />
          </div>
          <div className="rounded-lg border border-border bg-card p-4">
            <LabelTick>Artifacts</LabelTick>
            {!finished ? <p className="mt-3 text-sm text-muted-foreground">Written when the run finishes.</p> : (
              <ul className="mt-3 flex flex-col">
                {artifacts.data?.map((a) => (
                  <li key={a.name} className="flex h-10 items-center justify-between gap-3 border-b border-border last:border-0">
                    <span className="truncate font-mono text-xs text-foreground">{a.name}</span>
                    <span className="flex items-center gap-2">
                      <span className="font-mono text-xs text-muted-foreground">{formatBytes(a.size)}</span>
                      <Button variant="ghost" size="icon" asChild>
                        <a href={`/api/v1/jobs/${id}/artifacts/${a.name}`} download aria-label={`Download ${a.name}`}><Download size={16} strokeWidth={1.5} /></a>
                      </Button>
                    </span>
                  </li>
                ))}
              </ul>
            )}
          </div>
        </aside>
      </div>
    </>
  );
}
