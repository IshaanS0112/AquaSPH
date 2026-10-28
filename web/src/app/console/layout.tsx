import { Suspense } from "react";
import { ConsoleShell } from "@/components/console/shell";
import { Providers } from "@/components/providers";
import { Skeleton } from "@/components/ui/skeleton";

// The shell prerenders; page content that reads route params streams in behind the boundary.
export default function ConsoleLayout({ children }: { children: React.ReactNode }) {
  return (
    <Providers>
      <ConsoleShell>
        <Suspense fallback={<div className="flex flex-col gap-4 p-8"><Skeleton className="h-10 w-72" /><Skeleton className="h-64 w-full" /></div>}>
          {children}
        </Suspense>
      </ConsoleShell>
    </Providers>
  );
}
