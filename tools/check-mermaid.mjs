// Parses every ```mermaid block of the given markdown files with the Mermaid parser.
// Usage: node tools/check-mermaid.mjs docs/ARCHITECTURE.md   (needs: npm i mermaid jsdom)
import { readFileSync } from "node:fs";
import { JSDOM } from "jsdom";

const dom = new JSDOM("<!DOCTYPE html><body></body>");
globalThis.window = dom.window;
globalThis.document = dom.window.document;
const mermaid = (await import("mermaid")).default;
mermaid.initialize({ startOnLoad: false });

let failed = 0;
for (const file of process.argv.slice(2)) {
  const text = readFileSync(file, "utf8");
  const re = /```mermaid\n([\s\S]*?)```/g;
  let m, n = 0;
  while ((m = re.exec(text))) {
    n++;
    const line = text.slice(0, m.index).split("\n").length;
    try {
      await mermaid.parse(m[1]);
      console.log(`ok    ${file}:${line} block ${n}`);
    } catch (e) {
      failed++;
      console.log(`FAIL  ${file}:${line} block ${n}\n      ${String(e.message || e).split("\n").slice(0, 4).join("\n      ")}`);
    }
  }
}
process.exit(failed ? 1 : 0);
