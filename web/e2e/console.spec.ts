import { expect, test, type Page } from "@playwright/test";

// AQUASPH_E2E_KEY: an API key for a tenant on the stack under test.
const KEY = process.env.AQUASPH_E2E_KEY ?? "";
const SHOTS = process.env.SHOTS_DIR;

async function shot(page: Page, name: string) {
  if (!SHOTS) return;
  for (const [w, h] of [[1440, 900], [390, 844]] as const) {
    await page.setViewportSize({ width: w, height: h });
    await page.waitForTimeout(400);
    await page.screenshot({ path: `${SHOTS}/${name}-${w}.png`, fullPage: true });
  }
  await page.setViewportSize({ width: 1440, height: 900 });
}

async function signIn(page: Page) {
  await page.goto("/console");
  await expect(page).toHaveURL(/\/login/);
  await page.getByLabel("API key").fill(KEY);
  await page.getByRole("button", { name: "Sign in" }).click();
  await expect(page).toHaveURL(/\/console$/);
}

test.skip(!KEY, "set AQUASPH_E2E_KEY");

test("a wrong key is refused and the key never reaches page script", async ({ page }) => {
  await page.goto("/login");
  await page.getByLabel("API key").fill("aqk_aaaaaaaa_" + "A".repeat(32));
  await page.getByRole("button", { name: "Sign in" }).click();
  await expect(page.locator("#api-key-error")).toContainText("rejected");
  await signIn(page);
  expect(await page.evaluate(() => document.cookie)).not.toContain("aq_session");
});

test("landing page shows the renders", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByRole("heading", { level: 1 })).toContainText("fluid simulation");
  await shot(page, "landing");
});

test("console walk-through", async ({ page }) => {
  await signIn(page);
  await expect(page.getByText("Cache hit rate")).toBeVisible();
  await shot(page, "overview");

  await page.getByRole("link", { name: "Scenarios" }).first().click();
  await expect(page.getByRole("heading", { name: "Scenarios" })).toBeVisible();
  await shot(page, "scenarios");

  await page.goto("/console/scenarios/dam_break");
  await expect(page.getByRole("button", { name: "Run simulation" })).toBeVisible();
  await shot(page, "scenario-detail");

  await page.goto("/console/jobs?");
  await expect(page.getByRole("heading", { name: "Jobs" })).toBeVisible();
  await shot(page, "jobs");

  const done = page.getByRole("row").filter({ hasText: "completed" }).first();
  await expect(done).toBeVisible({ timeout: 90_000 });
  await done.click();
  await expect(page).toHaveURL(/\/console\/jobs\/.+/);
  await expect(page.locator("canvas")).toBeVisible({ timeout: 30_000 });
  await page.waitForTimeout(1500);
  await shot(page, "job-playback");
  await page.getByRole("tab", { name: "Physics" }).click();
  await expect(page.getByText("mechanical energy")).toBeVisible();
  await shot(page, "job-physics");

  await page.goto("/console/webhooks");
  await page.getByRole("button", { name: "Add endpoint" }).first().click();
  await page.getByLabel("HTTPS URL").fill("http://127.0.0.1:9/hook");
  await page.getByRole("button", { name: "Create" }).click();
  await expect(page.getByText("Store the signing secret now")).toBeVisible();
  await page.getByRole("button", { name: "Done" }).click();
  await page.getByRole("button", { name: "Send test" }).first().click();
  await expect(page.getByText("webhook.test")).toBeVisible({ timeout: 15_000 });
  await shot(page, "webhooks");

  await page.goto("/console/settings");
  await expect(page.getByText("Usage against limits")).toBeVisible();
  await shot(page, "settings");
});
