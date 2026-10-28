import type { Metadata, Viewport } from "next";
import { Geist, Geist_Mono } from "next/font/google";
import { Toaster } from "@/components/ui/sonner";
import "./globals.css";

const geist = Geist({ subsets: ["latin"], variable: "--font-geist" });
const geistMono = Geist_Mono({ subsets: ["latin"], variable: "--font-geist-mono" });

export const metadata: Metadata = {
  metadataBase: new URL(process.env.AQUASPH_PUBLIC_URL ?? "http://localhost:3000"),
  title: { default: "AquaSPH", template: "%s · AquaSPH" },
  description:
    "A deterministic 3D SPH fluid solver in C++17, run as a multi-tenant simulation platform in Go.",
  openGraph: {
    title: "AquaSPH",
    description: "Submit a fluid simulation over HTTP, watch it run, get machine-readable physics back.",
    images: ["/social-preview.png"],
  },
};

export const viewport: Viewport = {
  themeColor: "#0A0B0D",
  colorScheme: "dark",
};

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="en" className={`${geist.variable} ${geistMono.variable}`}>
      <body className="min-h-dvh">
        {children}
        <Toaster position="bottom-right" />
      </body>
    </html>
  );
}
