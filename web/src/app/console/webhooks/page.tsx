"use client";

import { useState } from "react";
import { Loader2, Plus, RotateCw, Send, Trash2 } from "lucide-react";
import { toast } from "sonner";
import { PageHeader } from "@/components/console/shell";
import { CopyButton, EmptyState, ErrorState, LabelTick, StatusBadge } from "@/components/console/bits";
import { Button } from "@/components/ui/button";
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle, DialogTrigger } from "@/components/ui/dialog";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { Skeleton } from "@/components/ui/skeleton";
import { useCreateWebhook, useDeleteWebhook, useRedeliver, useTestWebhook, useWebhookDeliveries, useWebhooks } from "@/lib/api/hooks";
import { ApiError } from "@/lib/api/client";
import { formatRelative } from "@/lib/format";
import { cn } from "@/lib/utils";

function CreateDialog() {
  const create = useCreateWebhook();
  const [open, setOpen] = useState(false);
  const [url, setUrl] = useState("");
  const [description, setDescription] = useState("");
  const [secret, setSecret] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const close = (o: boolean) => { setOpen(o); if (!o) { setSecret(null); setUrl(""); setDescription(""); setError(null); } };
  return (
    <Dialog open={open} onOpenChange={close}>
      <DialogTrigger asChild><Button><Plus size={16} strokeWidth={1.5} />Add endpoint</Button></DialogTrigger>
      <DialogContent>
        {secret ? (
          <>
            <DialogHeader><DialogTitle>Store the signing secret now</DialogTitle>
              <DialogDescription>It is shown once. Verify every delivery: HMAC-SHA256 of &quot;&lt;t&gt;.&lt;body&gt;&quot; must equal v1 in the AquaSPH-Signature header.</DialogDescription></DialogHeader>
            <div className="flex items-center gap-2 rounded-md border border-border-strong bg-surface-2 p-3"><code className="flex-1 truncate font-mono text-xs">{secret}</code><CopyButton value={secret} label="Copy secret" /></div>
            <DialogFooter><Button onClick={() => close(false)}>Done</Button></DialogFooter>
          </>
        ) : (
          <form className="flex flex-col gap-4" onSubmit={async (e) => {
            e.preventDefault(); setError(null);
            try { const ep = await create.mutateAsync({ url, description }); setSecret(ep.secret ?? null); }
            catch (err) { setError(err instanceof ApiError ? Object.values(err.fieldErrors()).join("; ") || err.message : "Failed"); }
          }}>
            <DialogHeader><DialogTitle>Add a webhook endpoint</DialogTitle>
              <DialogDescription>Job completed, failed and cancelled events are delivered at least once, signed, and retried with backoff.</DialogDescription></DialogHeader>
            <div className="flex flex-col gap-2"><Label htmlFor="wh-url">HTTPS URL</Label><Input id="wh-url" value={url} onChange={(e) => setUrl(e.target.value)} placeholder="https://hooks.example.com/aquasph" className="font-mono text-xs" /></div>
            <div className="flex flex-col gap-2"><Label htmlFor="wh-desc">Description</Label><Input id="wh-desc" value={description} onChange={(e) => setDescription(e.target.value)} placeholder="Slack relay" /></div>
            {error && <p role="alert" className="text-sm text-destructive">{error}</p>}
            <DialogFooter><Button type="submit" disabled={!url || create.isPending}>{create.isPending && <Loader2 size={16} className="animate-spin" />}Create</Button></DialogFooter>
          </form>
        )}
      </DialogContent>
    </Dialog>
  );
}

function Deliveries({ id }: { id: string }) {
  const ds = useWebhookDeliveries(id);
  const redeliver = useRedeliver(id);
  if (ds.isLoading) return <Skeleton className="h-24 w-full" />;
  if (ds.error) return <ErrorState error={ds.error} />;
  if (!ds.data?.length) return <p className="py-6 text-center text-sm text-muted-foreground">No deliveries yet. Send a test event, or wait for a job to finish.</p>;
  return (
    <div className="overflow-x-auto"><table className="w-full min-w-[640px] text-sm">
      <thead><tr className="h-10 border-b border-border text-left font-mono text-xs tracking-label text-muted-foreground uppercase">
        <th className="px-4 font-normal">Event</th><th className="px-4 font-normal">State</th><th className="px-4 text-right font-normal">Attempts</th>
        <th className="px-4 font-normal">Last response</th><th className="px-4 text-right font-normal">When</th><th className="px-4" /></tr></thead>
      <tbody>{ds.data.map((d) => (
        <tr key={d.id} className="h-10 border-b border-border last:border-0">
          <td className="px-4 font-mono text-xs">{d.event_type}</td>
          <td className="px-4"><StatusBadge state={d.state} /></td>
          <td className="px-4 text-right font-mono text-xs">{d.attempt}</td>
          <td className="max-w-64 truncate px-4 font-mono text-xs text-muted-foreground">{d.last_status ?? d.last_error ?? "—"}</td>
          <td className="px-4 text-right font-mono text-xs text-muted-foreground">{formatRelative(d.last_attempt_at ?? d.created_at)}</td>
          <td className="px-4 text-right">{(d.state === "dead" || d.state === "delivered") && (
            <Button variant="ghost" size="icon" aria-label="Redeliver" onClick={() => redeliver.mutate(d.id, { onSuccess: () => toast("Queued for redelivery") })}><RotateCw size={14} strokeWidth={1.5} /></Button>)}</td>
        </tr>))}</tbody></table></div>
  );
}

export default function WebhooksPage() {
  const hooks = useWebhooks();
  const del = useDeleteWebhook();
  const test = useTestWebhook();
  const [selected, setSelected] = useState<string | null>(null);
  const current = selected ?? hooks.data?.[0]?.id;
  return (
    <>
      <PageHeader title="Webhooks" description="Get told when jobs finish instead of polling. Up to 10 endpoints per tenant." action={<CreateDialog />} />
      <div className="flex flex-col gap-6 px-4 py-6 sm:px-8">
        {hooks.error ? <ErrorState error={hooks.error} onRetry={() => hooks.refetch()} /> : hooks.isLoading ? <Skeleton className="h-32 w-full" /> :
          hooks.data?.length === 0 ? <EmptyState title="No endpoints" body="Add an HTTPS endpoint to receive job.completed, job.failed and job.cancelled events." /> : (
          <>
            <ul className="flex flex-col rounded-lg border border-border bg-card">
              {hooks.data?.map((h) => (
                <li key={h.id} className={cn("flex min-h-14 flex-wrap items-center gap-3 border-b border-border px-4 py-2 last:border-0", h.id === current && "bg-brand-tint")}>
                  <button type="button" onClick={() => setSelected(h.id)} className="min-w-0 flex-1 text-left focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none">
                    <p className="truncate font-mono text-xs text-foreground">{h.url}</p>
                    <p className="text-xs text-muted-foreground">{h.description || h.event_types.join(", ")}</p>
                  </button>
                  <Button variant="secondary" size="sm" onClick={() => test.mutate(h.id, { onSuccess: () => toast("Test event queued") })}><Send size={14} strokeWidth={1.5} />Send test</Button>
                  <Button variant="ghost" size="icon" aria-label="Delete endpoint" onClick={() => { if (confirm(`Delete ${h.url}? Pending deliveries are dropped.`)) del.mutate(h.id); }}><Trash2 size={16} strokeWidth={1.5} /></Button>
                </li>
              ))}
            </ul>
            {current && <section className="rounded-lg border border-border bg-card"><div className="border-b border-border px-4 py-3"><LabelTick>Recent deliveries</LabelTick></div><Deliveries id={current} /></section>}
          </>
        )}
      </div>
    </>
  );
}
