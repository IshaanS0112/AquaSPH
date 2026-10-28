"use client";

import Link from "next/link";
import { useRouter } from "next/navigation";
import { Zap } from "lucide-react";
import { ProgressBar, StatusBadge } from "@/components/console/bits";
import { Skeleton } from "@/components/ui/skeleton";
import type { Job } from "@/lib/api/types";
import { durationBetween, formatDuration, formatRelative, scenarioLabel, shortId } from "@/lib/format";

export function JobsTable({ jobs, loading, compact = false }: { jobs?: Job[]; loading?: boolean; compact?: boolean }) {
  const router = useRouter();
  return (
    <div className="overflow-x-auto rounded-lg border border-border bg-card">
      <table className="w-full min-w-[640px] text-sm">
        <thead>
          <tr className="h-10 border-b border-border text-left font-mono text-xs tracking-label text-muted-foreground uppercase">
            <th className="px-4 font-normal">Job</th>
            <th className="px-4 font-normal">Scenario</th>
            <th className="px-4 font-normal">State</th>
            <th className="hidden px-4 font-normal sm:table-cell">Progress</th>
            <th className="px-4 text-right font-normal">Runtime</th>
            {!compact && <th className="hidden px-4 text-right font-normal md:table-cell">Quality</th>}
            <th className="px-4 text-right font-normal">Submitted</th>
          </tr>
        </thead>
        <tbody>
          {loading &&
            Array.from({ length: compact ? 5 : 8 }).map((_, i) => (
              <tr key={i} className="h-10 border-b border-border last:border-0">
                <td colSpan={compact ? 6 : 7} className="px-4">
                  <Skeleton className="h-4 w-full" />
                </td>
              </tr>
            ))}
          {!loading &&
            jobs?.map((j) => {
              const runtime = j.cache_hit ? null : durationBetween(j.started_at, j.finished_at);
              const fraction = j.state === "completed" ? 1 : (j.progress?.fraction ?? 0);
              return (
                <tr
                  key={j.id}
                  onClick={() => router.push(`/console/jobs/${j.id}`)}
                  className="h-10 cursor-pointer border-b border-border transition-colors duration-[120ms] last:border-0 hover:bg-surface-2"
                >
                  <td className="px-4">
                    <Link
                      href={`/console/jobs/${j.id}`}
                      onClick={(e) => e.stopPropagation()}
                      className="rounded-sm font-mono text-xs text-muted-foreground hover:text-foreground focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none"
                    >
                      {shortId(j.id)}
                    </Link>
                  </td>
                  <td className="px-4 whitespace-nowrap text-foreground capitalize">{scenarioLabel(j.scenario)}</td>
                  <td className="px-4">
                    <span className="inline-flex items-center gap-2">
                      <StatusBadge state={j.state} />
                      {j.cache_hit && (
                        <span className="inline-flex items-center gap-1 font-mono text-2xs text-muted-foreground" title="Served from the result cache">
                          <Zap size={12} strokeWidth={1.5} />
                          cache
                        </span>
                      )}
                    </span>
                  </td>
                  <td className="hidden w-40 px-4 sm:table-cell">
                    {j.state === "running" || j.state === "queued" ? (
                      <ProgressBar value={fraction} />
                    ) : (
                      <span className="font-mono text-xs text-subtle">{j.outcome ?? j.error?.code ?? "—"}</span>
                    )}
                  </td>
                  <td className="px-4 text-right font-mono text-xs whitespace-nowrap text-foreground tabular">
                    {j.cache_hit ? "0 ms" : formatDuration(runtime)}
                  </td>
                  {!compact && (
                    <td className="hidden px-4 text-right font-mono text-xs text-muted-foreground md:table-cell">{j.quality}</td>
                  )}
                  <td className="px-4 text-right font-mono text-xs whitespace-nowrap text-muted-foreground">
                    {formatRelative(j.created_at)}
                  </td>
                </tr>
              );
            })}
        </tbody>
      </table>
    </div>
  );
}
