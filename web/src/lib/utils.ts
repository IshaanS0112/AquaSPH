import { clsx, type ClassValue } from "clsx";
import { extendTailwindMerge } from "tailwind-merge";

// text-2xs is a font-size token from the design system; without this, tailwind-merge would
// read it as a colour and drop it when merged with text-muted-foreground.
const twMerge = extendTailwindMerge({ extend: { theme: { text: ["2xs"] } } });

export function cn(...inputs: ClassValue[]) {
  return twMerge(clsx(inputs));
}
