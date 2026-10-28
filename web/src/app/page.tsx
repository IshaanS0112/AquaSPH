import Image from "next/image";
import Link from "next/link";
import { ArrowRight } from "lucide-react";
import { Brand } from "@/components/brand";
import { Button } from "@/components/ui/button";
import { catalog } from "@/lib/catalog";

const GITHUB = "https://github.com/IshaanS0112/AquaSPH";

const proof = [
  ["15 ms", "a cache hit, against 11 s for a fresh low-quality run"],
  ["2,852 req/s", "API reads at p99 20 ms on one shared 4-core VM"],
  ["0 bytes", "of difference between runs at 1, 2, 4 and 8 threads"],
];

const steps = [
  ["01", "Submit", "POST a scenario and overrides. The spec is canonicalised and hashed; an identical request returns the stored result in milliseconds."],
  ["02", "Queue", "The Postgres jobs table is the queue: FOR UPDATE SKIP LOCKED, leases, and a fencing token on every write, so a stalled worker cannot overwrite a retry."],
  ["03", "Simulate", "A Go worker supervises the C++ solver as a process, relays its JSON progress over SSE, and records particle snapshots for playback."],
  ["04", "Deliver", "Results, metrics and snapshots become artifacts. A transactional outbox signs and delivers a webhook for every finished job."],
];

const deep = [
  {
    label: "Determinism",
    title: "The cache is only sound because the physics is bit-identical",
    body: "Reductions run over fixed-size chunks, emission is ordered and compaction stable, so a run’s output does not depend on the thread count. That is what makes a content-addressed cache keyed on the spec hash and the solver binary’s SHA-256 safe to serve.",
    code: "cache_key = sha256(canonical_spec) ⊕ sha256(aquasph binary)",
  },
  {
    label: "Failure",
    title: "A job survived two kill -9s of its worker",
    body: "Expired leases are requeued with jittered backoff by a reaper; the solver dies with its worker through Pdeathsig; a worker that loses its lease stops and writes nothing. Each of these is tested, and the queue’s locking was mutation-checked.",
    code: "UPDATE jobs … WHERE id = $1 AND lease_token = $2 AND state = 'running'",
  },
  {
    label: "Rendering",
    title: "Every image here is the solver’s own output",
    body: "Snapshots are splatted into an SPH density field and iso-surfaced with OpenVDB, then rendered in Blender Cycles. Nothing is re-simulated for looks: the water is where the particles were.",
    code: "render/blender/render_scene.py --snapshot run.aqsnap",
  },
];

export default function Landing() {
  const gallery = catalog.filter((s) => s.render);
  return (
    <div className="min-h-dvh">
      <header className="mx-auto flex h-16 max-w-[1200px] items-center justify-between px-4 sm:px-8">
        <Brand />
        <nav className="flex items-center gap-2">
          <Button variant="ghost" asChild className="hidden sm:inline-flex"><a href={GITHUB}>GitHub</a></Button>
          <Button asChild><Link href="/console">Open console</Link></Button>
        </nav>
      </header>

      <main>
        <section className="mx-auto max-w-[1200px] px-4 pt-12 pb-16 sm:px-8 sm:pt-20">
          <p className="label-tick">Fluid simulation as a service</p>
          <h1 className="mt-4 max-w-4xl text-4xl leading-[1.05] font-semibold tracking-display text-foreground sm:text-6xl">
            Submit a 3D fluid simulation over HTTP. Get physics back in seconds, or in 15 ms from cache.
          </h1>
          <p className="mt-6 max-w-2xl text-lg text-muted-foreground">
            A deterministic SPH solver in C++17, run as a multi-tenant platform in Go on Postgres and Redis, with a console that streams progress live and replays every run in 3D.
          </p>
          <div className="mt-8 flex flex-wrap gap-3">
            <Button size="lg" asChild><Link href="/console">Open the console <ArrowRight size={16} strokeWidth={1.5} /></Link></Button>
            <Button size="lg" variant="secondary" asChild><a href={GITHUB}>Read the code</a></Button>
          </div>
          <figure className="mt-12 overflow-hidden rounded-lg border border-border">
            <div className="relative aspect-video">
              <Image src="/renders/hero-dam-break.jpg" alt="A dam break 0.45 s after release, rendered in Blender Cycles from the solver's particle snapshot" fill priority sizes="(min-width:1200px) 1136px, 100vw" className="object-cover" />
            </div>
            <figcaption className="border-t border-border bg-card px-4 py-3 font-mono text-2xs text-muted-foreground">
              dam_break · 21,525 particles · t = 0.45 s · Blender Cycles render of the solver state
            </figcaption>
          </figure>
        </section>

        <section className="border-y border-border bg-card">
          <div className="mx-auto grid max-w-[1200px] grid-cols-1 gap-8 px-4 py-12 sm:grid-cols-3 sm:px-8">
            {proof.map(([n, t]) => (
              <div key={n}>
                <p className="num text-4xl font-medium text-foreground">{n}</p>
                <p className="mt-2 text-sm text-muted-foreground">{t}</p>
              </div>
            ))}
          </div>
          <p className="mx-auto max-w-[1200px] px-4 pb-8 font-mono text-2xs text-subtle sm:px-8">Measured: docs/platform/benchmarks.md and tests/test_determinism.cpp.</p>
        </section>

        <section className="mx-auto max-w-[1200px] px-4 py-24 sm:px-8">
          <p className="label-tick">How it works</p>
          <h2 className="mt-4 max-w-2xl text-3xl font-semibold tracking-display">From request to rendered result</h2>
          <ol className="mt-12 grid grid-cols-1 gap-px overflow-hidden rounded-lg border border-border bg-border md:grid-cols-4">
            {steps.map(([n, title, body]) => (
              <li key={n} className="bg-background p-6">
                <span className="font-mono text-xs text-primary">{n}</span>
                <h3 className="mt-3 text-lg font-medium text-foreground">{title}</h3>
                <p className="mt-2 text-sm text-muted-foreground">{body}</p>
              </li>
            ))}
          </ol>
        </section>

        <section className="mx-auto flex max-w-[1200px] flex-col gap-16 px-4 pb-24 sm:px-8">
          {deep.map((d, i) => (
            <div key={d.label} className="grid grid-cols-1 items-center gap-8 lg:grid-cols-2">
              <div className={i % 2 ? "lg:order-2" : undefined}>
                <p className="label-tick">{d.label}</p>
                <h3 className="mt-4 text-2xl font-semibold tracking-display">{d.title}</h3>
                <p className="mt-4 text-muted-foreground">{d.body}</p>
              </div>
              <pre className="rounded-lg border border-border bg-card p-6 font-mono text-sm whitespace-pre-wrap break-words text-foreground">{d.code}</pre>
            </div>
          ))}
        </section>

        <section className="border-t border-border">
          <div className="mx-auto max-w-[1200px] px-4 py-24 sm:px-8">
            <p className="label-tick">Scenario laboratory</p>
            <h2 className="mt-4 max-w-2xl text-3xl font-semibold tracking-display">Fourteen phenomena, zero lines of scenario code</h2>
            <p className="mt-4 max-w-2xl text-muted-foreground">Each is a JSON file composed from the same primitives. Tier 2 scenarios are labelled as visual experiments, not predictions.</p>
            <div className="mt-12 grid grid-cols-1 gap-4 sm:grid-cols-2 lg:grid-cols-3">
              {gallery.map((s) => (
                <figure key={s.name} className="overflow-hidden rounded-lg border border-border bg-card">
                  <div className="relative aspect-video">
                    <Image src={s.render!.src} alt={`${s.name} render`} fill sizes="(min-width:1024px) 33vw, (min-width:640px) 50vw, 100vw" className="object-cover" />
                  </div>
                  <figcaption className="flex items-center justify-between gap-2 px-4 py-3">
                    <span className="text-sm text-foreground capitalize">{s.name.replace(/_/g, " ")}</span>
                    <span className="font-mono text-2xs text-muted-foreground">tier {s.tier} · t = {s.render!.time} s</span>
                  </figcaption>
                </figure>
              ))}
            </div>
          </div>
        </section>

        <section className="border-t border-border">
          <div className="mx-auto max-w-[1200px] px-4 py-24 sm:px-8">
            <p className="label-tick">Trade-offs</p>
            <h2 className="mt-4 text-3xl font-semibold tracking-display">What it deliberately does not do</h2>
            <ul className="mt-8 grid grid-cols-1 gap-6 text-muted-foreground md:grid-cols-3">
              <li>Weakly-compressible SPH, not DFSPH: simpler and deterministic, at the cost of small timesteps.</li>
              <li>No GPU solver: the platform’s value is correctness under failure, and CPU OpenMP keeps results reproducible.</li>
              <li>Tier 2 floods and tsunamis are visual experiments at lab resolution, labelled as such everywhere.</li>
            </ul>
          </div>
        </section>
      </main>

      <footer className="border-t border-border">
        <div className="mx-auto flex max-w-[1200px] flex-wrap items-center justify-between gap-4 px-4 py-8 sm:px-8">
          <Brand />
          <p className="font-mono text-2xs text-muted-foreground">Built by Ishaan Singh · MIT licence · <a className="hover:text-foreground" href={GITHUB}>GitHub</a></p>
        </div>
      </footer>
    </div>
  );
}
