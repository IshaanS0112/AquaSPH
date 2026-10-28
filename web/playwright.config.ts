import { defineConfig } from "@playwright/test";

// Runs against a live stack (API, worker, console). CI starts it; locally: make up && npm run dev.
export default defineConfig({
  testDir: "e2e",
  timeout: 120_000,
  use: {
    baseURL: process.env.CONSOLE_URL ?? "http://localhost:3000",
    launchOptions: process.env.PW_CHROMIUM ? { executablePath: process.env.PW_CHROMIUM } : {},
    colorScheme: "dark",
  },
  reporter: [["list"]],
});
