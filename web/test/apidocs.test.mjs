// The API tab (apidocs.py) must carry docs/protokoll.md parts 2 and 3 into the built page.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const page = readFileSync(path.join(here, "..", "dist", "index.html"), "utf8");
const md = readFileSync(path.join(here, "..", "..", "docs", "protokoll.md"), "utf8");

test("the marker is replaced", () => {
  assert.ok(!page.includes("<!-- API_DOCS -->"));
  const tab = page.slice(page.indexOf('id="tab-api"'));
  assert.ok(tab.includes("<h2"), "no headings in the API tab");
});

test("every endpoint heading of part 2 and part 3 is on the page", () => {
  const start = md.indexOf("## 2.");
  const heads = md.slice(start).split("\n").filter((l) => /^#{2,3} /.test(l));
  assert.ok(heads.length >= 10);
  for (const h of heads) {
    const text = h.replace(/^#+ /, "").replace(/`/g, "");
    const plain = text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    const shown = page.replace(/<\/?code>/g, "");
    assert.ok(shown.includes(plain), "missing heading: " + text);
  }
});

test("tables and code blocks are HTML, not Markdown", () => {
  const tab = page.slice(page.indexOf('id="tab-api"'));
  assert.ok(tab.includes("<table>") && tab.includes("<pre><code>"));
  assert.ok(!/\n\|---/.test(tab), "a Markdown table was left as text");
  assert.ok(!tab.includes("```"), "a code fence was left as text");
  assert.equal((tab.match(/<ul>/g) || []).length, (tab.match(/<\/ul>/g) || []).length);
});
