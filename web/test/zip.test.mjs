// The ZIP export (js/zip.js) must be readable by a standard unzip (Python's zipfile here).
import { test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { makeZip, crc32 } from "../js/zip.js";
import { createHash } from "node:crypto";

test("crc32 matches zlib", () => {
  assert.equal(crc32(new TextEncoder().encode("123456789")), 0xcbf43926);
});

test("zip opens with a standard reader and holds the same bytes", () => {
  const flash = Uint8Array.from({ length: 65536 }, (_, i) => (i * 31) & 0xff);
  const zip = makeZip([
    { name: "backups.csv", data: "mac,barcode\r\n000002079350341C,7038010000000\r\n" },
    { name: "000002079350341C.bin", data: flash },
  ]);
  const file = path.join(mkdtempSync(path.join(tmpdir(), "zip-")), "t.zip");
  writeFileSync(file, zip);
  const out = execFileSync("python3", ["-c", `
import sys, zipfile, hashlib
z = zipfile.ZipFile(sys.argv[1])
assert z.testzip() is None
print(z.namelist())
print(hashlib.sha256(z.read("000002079350341C.bin")).hexdigest())
print(z.read("backups.csv").decode())
`, file], { encoding: "utf8" });
  assert.match(out, /'backups.csv', '000002079350341C.bin'/);
  assert.ok(out.includes(createHash("sha256").update(flash).digest("hex")));
  assert.ok(out.includes("7038010000000"));
});

