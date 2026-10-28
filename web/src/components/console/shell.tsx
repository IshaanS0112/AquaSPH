"use client";

import Link from "next/link";
import { usePathname, useRouter } from "next/navigation";
import { Suspense, useState } from "react";
import { useQuery } from "@tanstack/react-query";
import {
  BookOpen,
  Grid3x3,
  LayoutDashboard,
  ListChecks,
  LogOut,
  Menu,
  Settings,
  Waves,
  Webhook,
  type LucideIcon,
} from "lucide-react";
import { Brand } from "@/components/brand";
import { Button } from "@/components/ui/button";
import { Sheet, SheetContent, SheetHeader, SheetTitle, SheetTrigger } from "@/components/ui/sheet";
import { cn } from "@/lib/utils";

type NavItem = { href: string; label: string; icon: LucideIcon; exact?: boolean };

const NAV: NavItem[] = [
  { href: "/console", label: "Overview", icon: LayoutDashboard, exact: true },
  { href: "/console/scenarios", label: "Scenarios", icon: Waves },
  { href: "/console/jobs", label: "Jobs", icon: ListChecks },
  { href: "/console/sweeps", label: "Sweeps", icon: Grid3x3 },
  { href: "/console/webhooks", label: "Webhooks", icon: Webhook },
];

type SessionInfo = { authenticated: boolean; tenant?: string; key_prefix?: string };

function useSession() {
  return useQuery({
    queryKey: ["session"],
    queryFn: async () => {
      const res = await fetch("/api/session");
      return (await res.json()) as SessionInfo;
    },
    staleTime: 60_000,
  });
}

function NavLinks({ onNavigate }: { onNavigate?: () => void }) {
  const pathname = usePathname();
  const active = (item: NavItem) => (item.exact ? pathname === item.href : pathname.startsWith(item.href));
  return (
    <nav className="flex flex-col gap-1" aria-label="Console">
      {NAV.map((item) => {
        const on = active(item);
        return (
          <Link
            key={item.href}
            href={item.href}
            onClick={onNavigate}
            aria-current={on ? "page" : undefined}
            className={cn(
              "flex h-9 items-center gap-3 rounded-md px-3 text-sm transition-colors duration-[120ms] focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none",
              on ? "bg-brand-tint text-foreground" : "text-muted-foreground hover:bg-surface-2 hover:text-foreground",
            )}
          >
            <item.icon size={20} strokeWidth={1.5} className={on ? "text-primary" : undefined} />
            {item.label}
          </Link>
        );
      })}
    </nav>
  );
}

function SidebarFooter({ onNavigate }: { onNavigate?: () => void }) {
  const pathname = usePathname();
  const router = useRouter();
  const { data } = useSession();
  const signOut = async () => {
    await fetch("/api/session", { method: "DELETE" });
    router.push("/login");
  };
  const on = pathname.startsWith("/console/settings");
  return (
    <div className="flex flex-col gap-1 border-t border-border pt-4">
      <a
        href="/api/openapi"
        className="flex h-9 items-center gap-3 rounded-md px-3 text-sm text-muted-foreground transition-colors duration-[120ms] hover:bg-surface-2 hover:text-foreground focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none"
      >
        <BookOpen size={20} strokeWidth={1.5} />
        API reference
      </a>
      <Link
        href="/console/settings"
        onClick={onNavigate}
        aria-current={on ? "page" : undefined}
        className={cn(
          "flex h-9 items-center gap-3 rounded-md px-3 text-sm transition-colors duration-[120ms] focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none",
          on ? "bg-brand-tint text-foreground" : "text-muted-foreground hover:bg-surface-2 hover:text-foreground",
        )}
      >
        <Settings size={20} strokeWidth={1.5} className={on ? "text-primary" : undefined} />
        Settings
      </Link>
      <div className="mt-2 flex items-center justify-between gap-2 rounded-md px-3 py-2">
        <div className="min-w-0">
          <p className="truncate text-sm text-foreground">{data?.tenant || "—"}</p>
          <p className="truncate font-mono text-2xs text-muted-foreground">
            {data?.key_prefix ? `aqk_${data.key_prefix}_…` : " "}
          </p>
        </div>
        <Button variant="ghost" size="icon" aria-label="Sign out" onClick={signOut}>
          <LogOut size={16} strokeWidth={1.5} />
        </Button>
      </div>
    </div>
  );
}

export function ConsoleShell({ children }: { children: React.ReactNode }) {
  const [open, setOpen] = useState(false);
  return (
    <div className="flex min-h-dvh">
      <aside className="sticky top-0 hidden h-dvh w-60 shrink-0 flex-col border-r border-sidebar-border bg-sidebar px-3 py-4 lg:flex">
        <div className="px-3 pb-6">
          <Brand href="/console" />
        </div>
        <div className="flex-1">
          <Suspense><NavLinks /></Suspense>
        </div>
        <Suspense><SidebarFooter /></Suspense>
      </aside>

      <div className="flex min-w-0 flex-1 flex-col">
        <header className="sticky top-0 z-20 flex h-14 items-center gap-3 border-b border-border bg-background px-4 lg:hidden">
          <Sheet open={open} onOpenChange={setOpen}>
            <SheetTrigger asChild>
              <Button variant="ghost" size="icon" aria-label="Open navigation">
                <Menu size={20} strokeWidth={1.5} />
              </Button>
            </SheetTrigger>
            <SheetContent side="left" className="flex w-72 flex-col bg-sidebar px-3 py-4">
              <SheetHeader className="px-3 pb-4">
                <SheetTitle asChild>
                  <div>
                    <Brand href="/console" />
                  </div>
                </SheetTitle>
              </SheetHeader>
              <div className="flex-1">
                <Suspense><NavLinks onNavigate={() => setOpen(false)} /></Suspense>
              </div>
              <Suspense><SidebarFooter onNavigate={() => setOpen(false)} /></Suspense>
            </SheetContent>
          </Sheet>
          <Brand href="/console" />
        </header>
        <main className="min-w-0 flex-1">{children}</main>
      </div>
    </div>
  );
}

// Page header: title, one line of context, and the page's single primary action on the right.
export function PageHeader({
  title,
  description,
  action,
  eyebrow,
}: {
  title: React.ReactNode;
  description?: React.ReactNode;
  action?: React.ReactNode;
  eyebrow?: React.ReactNode;
}) {
  return (
    <div className="flex flex-col gap-4 border-b border-border px-4 py-6 sm:flex-row sm:items-end sm:justify-between sm:px-8">
      <div className="min-w-0">
        {eyebrow && <div className="mb-2">{eyebrow}</div>}
        <h1 className="text-2xl font-semibold tracking-display text-foreground">{title}</h1>
        {description && <p className="mt-1 max-w-2xl text-sm text-muted-foreground">{description}</p>}
      </div>
      {action && <div className="flex shrink-0 items-center gap-2">{action}</div>}
    </div>
  );
}
