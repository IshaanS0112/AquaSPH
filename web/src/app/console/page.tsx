"use client";

import Link from "next/link";
import { Plus } from "lucide-react";
import { PageHeader } from "@/components/console/shell";
import { EmptyState, ErrorState, Kpi, LabelTick, SectionTitle } from "@/components/console/bits";
import { ChartLegendRow, ThroughputChart, throughputClaim } from "@/components/console/throughput-chart";
import { JobsTable } from "@/components/console/jobs-table";
import { Button } from "@/components/ui/button";
import { Skeleton } from "@/components/ui/skeleton";
import { useJobs, useStats } from "@/lib/api/hooks";
import { formatDuration, formatNumber, formatPercent, scenarioLabel } from "@/lib/format";

export default function OverviewPage() {
  const stats = useStats(24);
  const jobs = useJobs({ limit: 8 });
  const s = stats.data;

  return (
    <>
      <PageHeader
        title="Overview"
        description={s ? `Tenant ${s.tenant.name} · last 24 hours, refreshed every 10 seconds` : "Last 24 hours"}
        action={
          <Button asChild>
            <Link href="/console/scenarios">
              <Plus size={16} strokeWidth={1.5} />
              New simulation
            </Link>
          </Button>
        }
      />
      <div className="flex flex-col gap-8 px-4 py-6 sm:px-8">
        {stats.error ? (
          <ErrorState error={stats.error} onRetry={() => stats.refetch()} />
        ) : (
          <section className="grid grid-cols-1 gap-4 sm:grid-cols-2 xl:grid-cols-4" aria-label="Key figures">
            <Kpi
              label="Completed"
              loading={!s}
              value={formatNumber(s?.window.completed)}
              detail={s && `${formatNumber(s.window.completed - s.window.cache_hits)} fresh · ${formatNumber(s.window.cache_hits)} from cache`}
            />
            <Kpi
              label="Cache hit rate"
              loading={!s}
              value={formatPercent(s?.window.cache_hit_rate, 0)}
              detail="of completed results, served in milliseconds"
            />
            <Kpi
              label="Fresh-run p95"
              loading={!s}
              value={formatDuration(s?.window.runtime_p95_s)}
              detail={s && `p50 ${formatDuration(s.window.runtime_p50_s)} · queue wait p95 ${formatDuration(s.window.queue_wait_p95_s)}`}
            />
            <Kpi
              label="Running now"
              loading={!s}
              value={s ? `${s.states.running}` : "—"}
              detail={s && `${s.states.queued} queued · ${s.workers.live} worker${s.workers.live === 1 ? "" : "s"}, ${s.workers.slots} slots`}
            />
          </section>
        )}

        <section className="grid grid-cols-1 gap-6 xl:grid-cols-12">
          <div className="rounded-lg border border-border bg-card p-4 xl:col-span-8">
            <div className="mb-4 flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
              <div>
                <LabelTick>Throughput</LabelTick>
                <h2 className="mt-2 text-base font-medium text-foreground">
                  {s ? throughputClaim(s) : <Skeleton className="h-5 w-72" />}
                </h2>
                <p className="text-xs text-muted-foreground">Results per hour, UTC</p>
              </div>
              <ChartLegendRow />
            </div>
            {s ? <ThroughputChart stats={s} /> : <Skeleton className="h-64 w-full" />}
          </div>

          <div className="rounded-lg border border-border bg-card p-4 xl:col-span-4">
            <LabelTick>Scenarios</LabelTick>
            <h2 className="mt-2 mb-4 text-base font-medium text-foreground">Most run in 24 h</h2>
            {!s && (
              <div className="flex flex-col gap-3">
                {Array.from({ length: 5 }).map((_, i) => (
                  <Skeleton key={i} className="h-8 w-full" />
                ))}
              </div>
            )}
            {s && s.scenarios.length === 0 && (
              <EmptyState title="Nothing has run yet" body="Completed runs appear here, grouped by scenario." className="py-8" />
            )}
            {s && s.scenarios.length > 0 && (
              <ul className="flex flex-col">
                {s.scenarios.map((sc) => (
                  <li key={sc.scenario} className="flex h-10 items-center justify-between gap-3 border-b border-border last:border-0">
                    <Link
                      href={`/console/scenarios/${sc.scenario}`}
                      className="truncate rounded-sm text-sm text-foreground capitalize hover:underline focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none"
                    >
                      {scenarioLabel(sc.scenario)}
                    </Link>
                    <span className="flex shrink-0 items-center gap-4 font-mono text-xs text-muted-foreground tabular">
                      <span title="Runs">{sc.runs}×</span>
                      <span title="Median fresh-run time" className="w-16 text-right text-foreground">
                        {formatDuration(sc.runtime_p50_s)}
                      </span>
                    </span>
                  </li>
                ))}
              </ul>
            )}
          </div>
        </section>

        <section>
          <SectionTitle
            title="Recent jobs"
            detail="Live: refreshes every 5 seconds"
            action={
              <Button variant="secondary" size="sm" asChild>
                <Link href="/console/jobs">All jobs</Link>
              </Button>
            }
          />
          {jobs.error ? (
            <ErrorState error={jobs.error} onRetry={() => jobs.refetch()} />
          ) : jobs.data?.data.length === 0 ? (
            <EmptyState
              title="No jobs yet"
              body="Pick a scenario and run it; its progress streams here live."
              action={
                <Button asChild>
                  <Link href="/console/scenarios">Browse scenarios</Link>
                </Button>
              }
            />
          ) : (
            <JobsTable jobs={jobs.data?.data} loading={jobs.isLoading} compact />
          )}
        </section>
      </div>
    </>
  );
}
