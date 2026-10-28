"use client";

import Image from "next/image";
import Link from "next/link";
import { PageHeader } from "@/components/console/shell";
import { TierBadge } from "@/components/console/bits";
import { catalog } from "@/lib/catalog";
import { scenarioLabel } from "@/lib/format";

export default function ScenariosPage() {
  return (
    <>
      <PageHeader
        title="Scenarios"
        description="Fourteen simulations built from the same JSON primitives. Each image is a Blender render of the solver's own output at the time shown."
      />
      <div className="grid grid-cols-1 gap-4 px-4 py-6 sm:grid-cols-2 sm:px-8 xl:grid-cols-3">
        {catalog.map((s) => (
          <Link
            key={s.name}
            href={`/console/scenarios/${s.name}`}
            className="group overflow-hidden rounded-lg border border-border bg-card transition-colors duration-[120ms] hover:border-border-strong focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none"
          >
            <div className="relative aspect-video bg-surface-2">
              {s.render && (
                <Image src={s.render.src} alt={`${s.name} render`} fill sizes="(min-width:1280px) 33vw, (min-width:640px) 50vw, 100vw" className="object-cover" />
              )}
            </div>
            <div className="flex flex-col gap-2 p-4">
              <div className="flex items-center justify-between gap-2">
                <h2 className="text-sm font-medium text-foreground capitalize">{scenarioLabel(s.name)}</h2>
                <TierBadge tier={s.tier} />
              </div>
              <p className="line-clamp-2 text-sm text-muted-foreground">{s.description}</p>
              {s.render && (
                <p className="font-mono text-2xs text-subtle">
                  t = {s.render.time} s · {s.render.quality} quality
                </p>
              )}
            </div>
          </Link>
        ))}
      </div>
    </>
  );
}
