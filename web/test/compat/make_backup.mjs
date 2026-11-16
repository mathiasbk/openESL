#!/usr/bin/env node
// Writes the dashboard backup file of a release for the compatibility test (#307).
//
//   node web/test/compat/make_backup.mjs v0.10.0
//
// Takes web/js of that release from git (git archive, nothing in the working tree changes),
// fills today's simulated AP (../fake_backup_ap.mjs) with settings, a layout file, a font file,
// tags with content, a picture, a tag's own settings and a scheduled entry, and saves what that
// release's makeBackup (the "Back up" button on the AP settings page) writes as
// web/test/compat/<release>-dashboard-backup.json. backup_compat.test.mjs then checks that
// today's restore takes it, and tools/esl-client/test_esl_client.py that esl-client's does.
// Run it once per release, in the VERSION PR, and commit the file; never write one for a
// release again. The dashboard has had backups since 0.9.0.
import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, existsSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { FakeAp } from "../fake_backup_ap.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..", "..");
export const FUTURE = 4102444800; // 2100-01-01: still ahead whenever the file is restored
const TAKEN = new Date("2026-11-01T12:00:00Z");
// a picture as the AP gives it back (GET /api/tags/{mac}/image): the raw 2.9" image, 296 x 128 bits
const PICTURE = Buffer.from(Uint8Array.from({ length: 4736 }, (_, i) => (i < 4608 ? i % 256 : 0))).toString("base64");

// Today's simulated AP with something of everything a backup holds
export function compatAp() {
  const ap = new FakeAp();
  ap.config = { ...ap.config, poll_s: 120, webhook: "http://192.0.2.5:8080/esl", tz: "CET-1CEST,M3.5.0,M10.5.0/3" };
  ap.layouts["2.9"] = {
    format: "esl-layouts", version: 1, model: "2.9",
    layouts: [{ name: "shelf", fields: ["name", "price", "unit"], elements: [
      { type: "text", field: "name", x: 0, y: 0, w: 296, h: 30 },
      { type: "text", field: "price", x: 0, y: 40, w: 296, h: 60 },
      { type: "text", field: "unit", x: 0, y: 100, w: 296, h: 20 },
    ] }],
  };
  ap.fonts = { text28: { format: "esl-font", version: 1, name: "text28", height: 10, baseline: 8, first: 65, last: 65, widths: [1], bitmap: "/8A=" } };
  Object.assign(ap.tag("00000123456789A8"), { model: "2.9", content: { layout: "shelf", fields: ["Milk 1 L", "24.90", "per l 24.90"], price_highlight: true } });
  Object.assign(ap.tag("00000123456789A9"), { model: "1.54", content: { layout: "text", fields: ["Open", "9-21"], frame: true }, config: { poll_s: 300 } });
  Object.assign(ap.tag("00000123456789AA"), { model: "2.9", content: { layout: "price", fields: ["Coffee 500 g", "49.90", "per kg 99.80"] } });
  Object.assign(ap.tag("00000123456789B1"), { model: "2.9", content: { layout: "image", fields: [], picture: PICTURE } });
  ap.tag("00000123456789B2").model = "1.54"; // has only polled: not in the backup
  ap.scheduled.push({ mac: "00000123456789A8", at: FUTURE, layout: "shelf", fields: ["Milk 1 L", "19.90", "Offer"], price_highlight: true, frame: false });
  return ap;
}

async function main(release) {
  if (!/^v\d+\.\d+\.\d+$/.test(release || "")) {
    console.error("usage: node web/test/compat/make_backup.mjs v<release>");
    return 2;
  }
  const tmp = mkdtempSync(join(tmpdir(), "openesl-backup-"));
  try {
    const tar = join(tmp, "src.tar");
    execFileSync("git", ["-C", ROOT, "archive", "-o", tar, release, "web/js", "shared"]);
    execFileSync("tar", ["-xf", tar, "-C", tmp]);
    const js = join(tmp, "web", "js", "backup.js");
    if (!existsSync(js)) {
      console.error(`${release} has no dashboard backup (web/js/backup.js, since 0.9.0)`);
      return 1;
    }
    const { makeBackup } = await import(pathToFileURL(js).href);
    const ap = compatAp();
    ap.fw = release.slice(1); // the files of v0.9.2 and v0.10.0 were written before this, with "0.8.1"
    const b = await makeBackup(ap, { now: () => TAKEN });
    const out = join(HERE, `${release}-dashboard-backup.json`);
    writeFileSync(out, JSON.stringify(b, null, 2) + "\n");
    console.log(`web/test/compat/${release}-dashboard-backup.json written`);
    return 0;
  } finally {
    rmSync(tmp, { recursive: true, force: true });
  }
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) process.exitCode = await main(process.argv[2]);
