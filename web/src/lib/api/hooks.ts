"use client";

import { useEffect, useRef, useState } from "react";
import { keepPreviousData, useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { api } from "./client";
import type {
  Artifact,
  Job,
  JobEvent,
  JobState,
  Metrics,
  Page,
  Progress,
  ScenarioDetail,
  ScenarioSummary,
  Stats,
  Sweep,
  SweepResults,
  WebhookDelivery,
  WebhookEndpoint,
} from "./types";

const terminal = (s?: JobState) => s === "completed" || s === "failed" || s === "cancelled";

export function useStats(hours = 24) {
  return useQuery({
    queryKey: ["stats", hours],
    queryFn: ({ signal }) => api<Stats>(`/stats?hours=${hours}`, { signal }),
    refetchInterval: 10_000,
  });
}

export function useScenarios() {
  return useQuery({
    queryKey: ["scenarios"],
    queryFn: ({ signal }) => api<{ data: ScenarioSummary[] }>("/scenarios", { signal }).then((r) => r.data),
    staleTime: 5 * 60_000,
  });
}

export function useScenario(name: string) {
  return useQuery({
    queryKey: ["scenario", name],
    queryFn: ({ signal }) => api<ScenarioDetail>(`/scenarios/${encodeURIComponent(name)}`, { signal }),
    staleTime: 5 * 60_000,
  });
}

export type JobFilter = { state?: JobState; scenario?: string; sweepId?: string; cursor?: string; limit?: number };

export function useJobs(f: JobFilter = {}, live = true) {
  const q = new URLSearchParams();
  if (f.state) q.set("state", f.state);
  if (f.scenario) q.set("scenario", f.scenario);
  if (f.sweepId) q.set("sweep_id", f.sweepId);
  if (f.cursor) q.set("cursor", f.cursor);
  q.set("limit", String(f.limit ?? 25));
  return useQuery({
    queryKey: ["jobs", f],
    queryFn: ({ signal }) => api<Page<Job>>(`/jobs?${q}`, { signal }),
    placeholderData: keepPreviousData,
    refetchInterval: live ? 5_000 : false,
  });
}

export function useJob(id: string) {
  return useQuery({
    queryKey: ["job", id],
    queryFn: ({ signal }) => api<Job>(`/jobs/${id}?include=spec`, { signal }),
    refetchInterval: (query) => (terminal(query.state.data?.state) ? false : 5_000),
  });
}

export function useJobHistory(id: string, state?: JobState) {
  return useQuery({
    queryKey: ["job-history", id, state],
    queryFn: ({ signal }) => api<{ events: JobEvent[] }>(`/jobs/${id}/history`, { signal }).then((r) => r.events),
  });
}

export function useJobArtifacts(id: string, enabled: boolean) {
  return useQuery({
    queryKey: ["job-artifacts", id],
    queryFn: ({ signal }) => api<{ data: Artifact[] }>(`/jobs/${id}/artifacts`, { signal }).then((r) => r.data),
    enabled,
  });
}

export function useJobMetrics(id: string, enabled: boolean) {
  return useQuery({
    queryKey: ["job-metrics", id],
    queryFn: ({ signal }) =>
      api<{ metrics: Metrics; partial: boolean }>(`/jobs/${id}/result`, { signal }),
    enabled,
    staleTime: Infinity,
  });
}

export type SubmitJob = {
  scenario: string;
  quality: string;
  sim_time?: number;
  overrides?: Record<string, unknown>;
  labels?: Record<string, string>;
  priority?: number;
};

export function useSubmitJob() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: SubmitJob) =>
      api<Job>("/jobs", { method: "POST", body, headers: { "idempotency-key": crypto.randomUUID() } }),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ["jobs"] });
      qc.invalidateQueries({ queryKey: ["stats"] });
    },
  });
}

export function useCancelJob(id: string) {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: () => api<Job>(`/jobs/${id}/cancel`, { method: "POST" }),
    onSuccess: (job) => {
      qc.setQueryData(["job", id], (old: Job | undefined) => (old ? { ...old, ...job, spec: old.spec } : job));
      qc.invalidateQueries({ queryKey: ["jobs"] });
    },
  });
}

export type LiveJob = { state?: JobState; progress?: Progress; connected: boolean };

// Follows /v1/jobs/{id}/events: a snapshot, then progress and state changes as they happen.
export function useJobEvents(id: string, enabled: boolean): LiveJob {
  const qc = useQueryClient();
  const [live, setLive] = useState<LiveJob>({ connected: false });
  const done = useRef(false);

  useEffect(() => {
    if (!enabled) return;
    done.current = false;
    const es = new EventSource(`/api/v1/jobs/${id}/events`);
    es.onopen = () => setLive((l) => ({ ...l, connected: true }));
    es.addEventListener("snapshot", (e) => {
      const job = JSON.parse((e as MessageEvent).data) as Job;
      setLive({ state: job.state, progress: job.progress ?? undefined, connected: true });
    });
    es.addEventListener("progress", (e) => {
      const p = JSON.parse((e as MessageEvent).data) as Progress;
      setLive((l) => ({ ...l, progress: p }));
    });
    es.addEventListener("state", (e) => {
      const { state } = JSON.parse((e as MessageEvent).data) as { state: JobState };
      setLive((l) => ({ ...l, state }));
    });
    es.addEventListener("done", (e) => {
      done.current = true;
      const job = JSON.parse((e as MessageEvent).data) as Job;
      setLive({ state: job.state, progress: job.progress ?? undefined, connected: false });
      qc.invalidateQueries({ queryKey: ["job", id] });
      qc.invalidateQueries({ queryKey: ["job-history", id] });
      qc.invalidateQueries({ queryKey: ["jobs"] });
      es.close();
    });
    es.onerror = () => {
      // EventSource reconnects on its own; after "done" the close is intentional.
      if (done.current) es.close();
      else setLive((l) => ({ ...l, connected: false }));
    };
    return () => es.close();
  }, [id, enabled, qc]);

  return live;
}

export function useSweeps() {
  return useQuery({
    queryKey: ["sweeps"],
    queryFn: ({ signal }) => api<Page<Sweep>>("/sweeps?limit=50", { signal }),
    refetchInterval: 10_000,
  });
}

export function useSweep(id: string) {
  return useQuery({
    queryKey: ["sweep", id],
    queryFn: ({ signal }) => api<Sweep>(`/sweeps/${id}`, { signal }),
    refetchInterval: (query) => (query.state.data?.state === "completed" ? false : 5_000),
  });
}

export function useSweepResults(id: string, metrics: string[], refresh: boolean) {
  return useQuery({
    queryKey: ["sweep-results", id, metrics],
    queryFn: ({ signal }) =>
      api<SweepResults>(`/sweeps/${id}/results?metrics=${encodeURIComponent(metrics.join(","))}`, { signal }),
    refetchInterval: refresh ? 5_000 : false,
  });
}

export function useCreateSweep() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: SubmitJob & { grid: Record<string, unknown[]> }) =>
      api<Sweep>("/sweeps", { method: "POST", body, headers: { "idempotency-key": crypto.randomUUID() } }),
    onSuccess: () => qc.invalidateQueries({ queryKey: ["sweeps"] }),
  });
}

export function useCancelSweep(id: string) {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: () => api<Sweep>(`/sweeps/${id}/cancel`, { method: "POST" }),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ["sweep", id] });
      qc.invalidateQueries({ queryKey: ["sweeps"] });
    },
  });
}

export function useWebhooks() {
  return useQuery({
    queryKey: ["webhooks"],
    queryFn: ({ signal }) => api<{ data: WebhookEndpoint[] }>("/webhooks", { signal }).then((r) => r.data),
  });
}

export function useWebhookDeliveries(id: string | undefined) {
  return useQuery({
    queryKey: ["webhook-deliveries", id],
    queryFn: ({ signal }) =>
      api<{ data: WebhookDelivery[] }>(`/webhooks/${id}/deliveries?limit=50`, { signal }).then((r) => r.data),
    enabled: !!id,
    refetchInterval: 5_000,
  });
}

export function useCreateWebhook() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: { url: string; description?: string; event_types?: string[] }) =>
      api<WebhookEndpoint>("/webhooks", { method: "POST", body }),
    onSuccess: () => qc.invalidateQueries({ queryKey: ["webhooks"] }),
  });
}

export function useDeleteWebhook() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (id: string) => api<void>(`/webhooks/${id}`, { method: "DELETE" }),
    onSuccess: () => qc.invalidateQueries({ queryKey: ["webhooks"] }),
  });
}

export function useTestWebhook() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (id: string) => api<{ event_id: string }>(`/webhooks/${id}/test`, { method: "POST" }),
    onSuccess: (_, id) => qc.invalidateQueries({ queryKey: ["webhook-deliveries", id] }),
  });
}

export function useRedeliver(endpointId: string) {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (deliveryId: number) =>
      api<unknown>(`/webhooks/${endpointId}/deliveries/${deliveryId}/redeliver`, { method: "POST" }),
    onSuccess: () => qc.invalidateQueries({ queryKey: ["webhook-deliveries", endpointId] }),
  });
}
