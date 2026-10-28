import { defineConfig } from "vitest/config";
import { fileURLToPath } from "node:url";

export default defineConfig({
  resolve: { alias: { "@": fileURLToPath(new URL("./src", import.meta.url)), "server-only": fileURLToPath(new URL("./src/test/empty.ts", import.meta.url)) } },
  test: { include: ["src/**/*.test.ts"], environment: "node" },
});
