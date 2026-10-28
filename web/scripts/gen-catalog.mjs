// Copies scenario metadata from configs/scenarios and the render settings from
// render/blender/shots.json into src/lib/catalog.generated.json, so the landing page and the
// scenario gallery render statically, without the API. Run: npm run gen:catalog
import { readFileSync, readdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const root = new URL("../../", import.meta.url).pathname;
const dir = join(root, "configs/scenarios");
const shots = JSON.parse(readFileSync(join(root, "render/blender/shots.json"), "utf8"));

const scenarios = readdirSync(dir)
  .filter((f) => f.endsWith(".json"))
  .sort()
  .map((f) => {
    const s = JSON.parse(readFileSync(join(dir, f), "utf8"));
    const extent = s.domain.max.map((v, i) => +(v - s.domain.min[i]).toFixed(3));
    return {
      name: s.name,
      tier: s.tier,
      description: s.description,
      approximation: s.approximation ?? "",
      simulatedTime: s.duration?.simulated_time ?? null,
      domain: extent,
      materials: (s.materials ?? []).map((m) => m.name),
      emitters: (s.emitters ?? []).length,
      obstacles: (s.obstacles ?? []).length,
      waveGenerators: (s.wave_generators ?? []).length,
      render: shots[s.name] ? { src: `/renders/${s.name}.jpg`, ...shots[s.name] } : null,
    };
  });

writeFileSync(
  new URL("../src/lib/catalog.generated.json", import.meta.url),
  JSON.stringify({ generatedFrom: "configs/scenarios + render/blender/shots.json", scenarios }, null, 2) + "\n",
);
console.log(`catalog: ${scenarios.length} scenarios`);
