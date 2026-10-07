// scripts/run-tests.mjs
//
// Execute every test binary under build/bin/ and report a summary.
//
//   node scripts/run-tests.mjs            run all
//   node scripts/run-tests.mjs meta bytemap   run only matching names
//
// Test binaries are built by `build.mjs --test`. A binary's exit status is
// its verdict; stdout/stderr are streamed through unchanged so a failure can
// be read in place.

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { logger } from "./lib/util.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const BIN = path.join(ROOT, "build", "bin");

const ui = logger("test");
const filters = process.argv.slice(2).filter((a) => !a.startsWith("-"));

if (!fs.existsSync(BIN)) {
    ui.fatal("no build/bin/ — run: node scripts/build.mjs --test");
}

const all = fs
    .readdirSync(BIN)
    .filter((f) => !f.startsWith("."))
    .sort();

const bins = filters.length
    ? all.filter((f) => filters.some((p) => f.includes(p)))
    : all;

if (bins.length === 0) {
    ui.fatal(
        filters.length
            ? `no test binary matches: ${filters.join(", ")}`
            : "no test binaries found — run: node scripts/build.mjs --test",
    );
}

ui.log(`running ${bins.length} test binar${bins.length === 1 ? "y" : "ies"}`);

const failed = [];
for (const name of bins) {
    const bin = path.join(BIN, name);
    process.stdout.write(`\n── ${name} ${"─".repeat(Math.max(0, 60 - name.length))}\n`);

    const r = spawnSync(bin, [], { stdio: "inherit" });
    if (r.status !== 0) failed.push({ name, status: r.status });
}

console.log("");
if (failed.length > 0) {
    ui.log(`${bins.length - failed.length}/${bins.length} passed`);
    for (const f of failed) {
        console.error(`       FAIL ${f.name} (exit ${f.status})`);
    }
    process.exit(1);
}

ui.log(`all ${bins.length} passed`);
