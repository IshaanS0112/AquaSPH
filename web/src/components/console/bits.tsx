"use client";

import { useState } from "react";
import { AlertTriangle, Check, Copy, RotateCw } from "lucide-react";
import { Button } from "@/components/ui/button";
import type { JobState } from "@/lib/api/types";
import { ApiError } from "@/lib/api/client";
import { cn } from "@/lib/utils";

// Status is never colour alone: every badge carries its word.
const STATE_STYLE: Record<JobState | "pending" | "delivering" | "delivered" | "dead" | "running_sweep", string> = {
  queued: "bg-surface-2 text-muted-foreground border-border-strong",
  running: "bg-info/12 text-info border-info/30",
  completed: "bg-success/12 text-success border-success/30",
  failed: "bg-destructive/12 text-destructive border-destructive/30",
  cancelled: "bg-surface-2 text-muted-foreground border-border",
  pending: "bg-surface-2 text-muted-foreground border-border-strong",
  delivering: "bg-info/12 text-info border-info/30",
  delivered: "bg-success/12 text-success border-success/30",
  dead: "bg-destructive/12 text-destructive border-destructive/30",
  running_sweep: "bg-info/12 text-info border-info/30",
};

export function StatusBadge({ state, className }: { state: keyof typeof STATE_STYLE; className?: string }) {
  const label = state === "running_sweep" ? "running" : state;
  return (
    <span
      className={cn(
        "inline-flex h-5 items-center gap-1.5 rounded-full border px-2 font-mono text-2xs whitespace-nowrap",
        STATE_STYLE[state],
        className,
      )}
    >
      {(state === "running" || state === "delivering" || state === "running_sweep") && (
        <span className="size-1.5 animate-pulse rounded-full bg-current motion-reduce:animate-none" aria-hidden="true" />
      )}
      {label}
    </span>
  );
}

export function TierBadge({ tier }: { tier: number }) {
  return (
    <span
      className={cn(
        "inline-flex h-5 items-center rounded-full border px-2 font-mono text-2xs whitespace-nowrap",
        tier === 1 ? "border-success/30 bg-success/12 text-success" : "border-warning/30 bg-warning/12 text-warning",
      )}
      title={tier === 1 ? "Physically demonstrable" : "Large-scale visual experiment, not predictive"}
    >
      tier {tier}
    </span>
  );
}

export function LabelTick({ children, className }: { children: React.ReactNode; className?: string }) {
  return <span className={cn("label-tick", className)}>{children}</span>;
}

export function Kpi({
  label,
  value,
  detail,
  loading,
}: {
  label: string;
  value: React.ReactNode;
  detail?: React.ReactNode;
  loading?: boolean;
}) {
  return (
    <div className="flex min-w-0 flex-col gap-2 rounded-lg border border-border bg-card p-4">
      <LabelTick>{label}</LabelTick>
      {loading ? (
        <div className="h-9 w-24 animate-pulse rounded-md bg-surface-2 motion-reduce:animate-none" />
      ) : (
        <div className="num text-3xl font-medium text-foreground">{value}</div>
      )}
      <div className="min-h-4 text-xs text-muted-foreground">{loading ? " " : detail}</div>
    </div>
  );
}

export function EmptyState({
  title,
  body,
  action,
  className,
}: {
  title: string;
  body?: React.ReactNode;
  action?: React.ReactNode;
  className?: string;
}) {
  return (
    <div
      className={cn(
        "dot-grid flex flex-col items-center justify-center gap-3 rounded-lg border border-dashed border-border-strong px-6 py-12 text-center",
        className,
      )}
    >
      <p className="text-sm font-medium text-foreground">{title}</p>
      {body && <p className="max-w-md text-sm text-muted-foreground">{body}</p>}
      {action}
    </div>
  );
}

export function ErrorState({ error, onRetry, className }: { error: unknown; onRetry?: () => void; className?: string }) {
  const [open, setOpen] = useState(false);
  const api = error instanceof ApiError ? error : null;
  const message =
    api?.status === 404
      ? "Not found. It may belong to another tenant, or the link is wrong."
      : api?.problem.detail ?? (error instanceof Error ? error.message : "Something went wrong.");
  return (
    <div
      role="alert"
      className={cn("flex flex-col gap-3 rounded-lg border border-destructive/30 bg-destructive/8 p-4", className)}
    >
      <div className="flex items-start gap-3">
        <AlertTriangle size={16} strokeWidth={1.5} className="mt-0.5 shrink-0 text-destructive" />
        <p className="text-sm text-foreground">{message}</p>
      </div>
      <div className="flex items-center gap-2 pl-7">
        {onRetry && (
          <Button size="sm" variant="secondary" onClick={onRetry}>
            <RotateCw size={14} strokeWidth={1.5} />
            Retry
          </Button>
        )}
        {api && (
          <button
            type="button"
            onClick={() => setOpen((o) => !o)}
            className="text-xs text-muted-foreground underline-offset-4 hover:underline focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none"
          >
            {open ? "Hide details" : "Details"}
          </button>
        )}
      </div>
      {open && api && (
        <pre className="ml-7 overflow-x-auto rounded-md bg-surface-2 p-3 font-mono text-xs text-muted-foreground">
          {JSON.stringify(api.problem, null, 2)}
        </pre>
      )}
    </div>
  );
}

export function CopyButton({ value, label = "Copy" }: { value: string; label?: string }) {
  const [copied, setCopied] = useState(false);
  return (
    <Button
      type="button"
      variant="ghost"
      size="icon"
      aria-label={label}
      onClick={async () => {
        await navigator.clipboard.writeText(value);
        setCopied(true);
        setTimeout(() => setCopied(false), 1500);
      }}
    >
      {copied ? <Check size={16} strokeWidth={1.5} className="text-success" /> : <Copy size={16} strokeWidth={1.5} />}
    </Button>
  );
}

export function KeyValues({ items, className }: { items: [string, React.ReactNode][]; className?: string }) {
  return (
    <dl className={cn("grid grid-cols-[auto_1fr] gap-x-6 gap-y-3 text-sm", className)}>
      {items.map(([k, v]) => (
        <div key={k} className="contents">
          <dt className="text-muted-foreground">{k}</dt>
          <dd className="min-w-0 truncate text-right font-mono text-foreground tabular">{v}</dd>
        </div>
      ))}
    </dl>
  );
}

export function ProgressBar({ value, className, label }: { value: number; className?: string; label?: string }) {
  const pct = Math.max(0, Math.min(1, value)) * 100;
  return (
    <div
      role="progressbar"
      aria-label={label ?? "Progress"}
      aria-valuemin={0}
      aria-valuemax={100}
      aria-valuenow={Math.round(pct)}
      className={cn("h-1.5 w-full overflow-hidden rounded-full bg-surface-2", className)}
    >
      <div
        className="h-full rounded-full bg-primary transition-[width] duration-[240ms] ease-out-quint motion-reduce:transition-none"
        style={{ width: `${pct}%` }}
      />
    </div>
  );
}

export function SectionTitle({ title, detail, action }: { title: string; detail?: string; action?: React.ReactNode }) {
  return (
    <div className="mb-3 flex items-end justify-between gap-4">
      <div className="min-w-0">
        <h2 className="text-base font-medium text-foreground">{title}</h2>
        {detail && <p className="text-xs text-muted-foreground">{detail}</p>}
      </div>
      {action}
    </div>
  );
}
