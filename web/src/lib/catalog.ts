import data from "./catalog.generated.json";

export type CatalogEntry = (typeof data.scenarios)[number];
export const catalog: CatalogEntry[] = data.scenarios;
export const catalogByName = new Map(catalog.map((s) => [s.name, s]));
