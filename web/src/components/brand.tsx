import Link from "next/link";
import { cn } from "@/lib/utils";

// The mark: three particles on a lattice, one of them in the accent, rising like a surge front.
export function BrandMark({ className }: { className?: string }) {
  return (
    <svg viewBox="0 0 24 24" aria-hidden="true" className={cn("size-5", className)}>
      <rect x="1" y="1" width="22" height="22" rx="6" className="fill-surface-2 stroke-border-strong" strokeWidth="1" />
      <circle cx="7.5" cy="15.5" r="2.25" className="fill-muted-foreground" />
      <circle cx="12" cy="11.5" r="2.25" className="fill-foreground" />
      <circle cx="16.5" cy="8" r="2.25" className="fill-primary" />
    </svg>
  );
}

export function Brand({ href = "/", className }: { href?: string; className?: string }) {
  return (
    <Link
      href={href}
      className={cn(
        "inline-flex items-center gap-2 rounded-md font-display text-sm font-semibold tracking-display text-foreground focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none",
        className,
      )}
    >
      <BrandMark />
      AquaSPH
    </Link>
  );
}
