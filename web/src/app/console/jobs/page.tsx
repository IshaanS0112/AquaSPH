"use client";

import { useState } from "react";
import { PageHeader } from "@/components/console/shell";
import { EmptyState, ErrorState } from "@/components/console/bits";
import { JobsTable } from "@/components/console/jobs-table";
import { Button } from "@/components/ui/button";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { useJobs } from "@/lib/api/hooks";
import type { JobState } from "@/lib/api/types";
import { catalog } from "@/lib/catalog";
import { scenarioLabel } from "@/lib/format";

const STATES: JobState[] = ["queued", "running", "completed", "failed", "cancelled"];

export default function JobsPage() {
  const [state, setState] = useState<string>("all");
  const [scenario, setScenario] = useState<string>("all");
  const [cursors, setCursors] = useState<string[]>([]);
  const jobs = useJobs({
    state: state === "all" ? undefined : (state as JobState),
    scenario: scenario === "all" ? undefined : scenario,
    cursor: cursors.at(-1),
  });
  const reset = () => setCursors([]);

  return (
    <>
      <PageHeader title="Jobs" description="Every simulation your tenant has submitted, newest first. Live." />
      <div className="flex flex-col gap-4 px-4 py-6 sm:px-8">
        <div className="flex flex-wrap items-center gap-3">
          <Select value={state} onValueChange={(v) => { setState(v); reset(); }}>
            <SelectTrigger className="w-40" aria-label="Filter by state"><SelectValue /></SelectTrigger>
            <SelectContent>
              <SelectItem value="all">All states</SelectItem>
              {STATES.map((s) => <SelectItem key={s} value={s}>{s}</SelectItem>)}
            </SelectContent>
          </Select>
          <Select value={scenario} onValueChange={(v) => { setScenario(v); reset(); }}>
            <SelectTrigger className="w-52" aria-label="Filter by scenario"><SelectValue /></SelectTrigger>
            <SelectContent>
              <SelectItem value="all">All scenarios</SelectItem>
              {catalog.map((s) => <SelectItem key={s.name} value={s.name} className="capitalize">{scenarioLabel(s.name)}</SelectItem>)}
            </SelectContent>
          </Select>
        </div>
        {jobs.error ? (
          <ErrorState error={jobs.error} onRetry={() => jobs.refetch()} />
        ) : jobs.data?.data.length === 0 ? (
          <EmptyState title="No jobs match" body="Change the filters, or run a scenario." />
        ) : (
          <JobsTable jobs={jobs.data?.data} loading={jobs.isLoading} />
        )}
        <div className="flex justify-end gap-2">
          <Button variant="secondary" size="sm" disabled={cursors.length === 0} onClick={() => setCursors((c) => c.slice(0, -1))}>Previous</Button>
          <Button variant="secondary" size="sm" disabled={!jobs.data?.next_cursor} onClick={() => setCursors((c) => [...c, jobs.data!.next_cursor!])}>Next</Button>
        </div>
      </div>
    </>
  );
}
