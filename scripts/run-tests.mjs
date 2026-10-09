// scripts/run-tests.mjs
//
// Execute every test binary under build/bin/ and report a summary.
//
//   node scripts/run-tests.mjs                run all
//   node scripts/run-tests.mjs meta bytemap   run only matching names
//
// Test binaries are built by `build.mjs --test`. A binary's exit status is
// its verdict; stdout/stderr are streamed through unchanged so a failure can
// be read in place.
//
// Every binary runs under a timeout. A collector test can deadlock — a
// logging macro that never terminates, a lock that is taken twice — and
// without a deadline that hangs CI forever instead of failing it. A timeout
// is reported as a failure, with the name of the binary that hung.

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { logger } from "./lib/util.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const BIN = path.join(ROOT, "build", "bin");

/* Default deadline per binary. Generous: the concurrent tests run hundreds
 * of thousands of operations, and a loaded machine is slow, but a real hang
 * is caught well inside a minute. Override with CVM_TEST_TIMEOUT_MS. */
const DEFAULT_TIMEOUT_MS = 60000;
const TIMEOUT_MS = Number(process.env.CVM_TEST_TIMEOUT_MS || DEFAULT_TIMEOUT_MS);

const ui = logger("test");
const filters = process.argv.slice(2).filter((a) => !a.startsWith("-"));

/*
 * ThreadSanitizer suppressions, when a sanitizer build is what is being run.
 *
 * Which build it is can be told from the binary's own name? No — the test
 * binaries are named after their source. It is told from the environment:
 * build.mjs leaves the marker below for exactly this purpose.
 */
const suppressions = path.join(ROOT, "tests", "tsan.supp");
const configFile = path.join(ROOT, "build", "config.json");
const buildConfig = fs.existsSync(configFile)
    ? JSON.parse(fs.readFileSync(configFile, "utf8"))
    : {};
const sanEnv = {};
if (buildConfig.sanitizer === "tsan" && fs.existsSync(suppressions)) {
    sanEnv.TSAN_OPTIONS =
        `${process.env.TSAN_OPTIONS ?? ""} suppressions=${suppressions}`.trim();
    ui.log(`tsan suppressions: ${path.relative(ROOT, suppressions)}`);
}

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

ui.log(
    `running ${bins.length} test binar${bins.length === 1 ? "y" : "ies"}` +
        ` (timeout ${TIMEOUT_MS}ms each)`,
);

const failed = [];
for (const name of bins) {
    const bin = path.join(BIN, name);
    process.stdout.write(`\n── ${name} ${"─".repeat(Math.max(0, 60 - name.length))}\n`);

    const r = spawnSync(bin, [], {
        stdio: "inherit",
        timeout: TIMEOUT_MS,
        env: { ...process.env, ...sanEnv },
    });

    const timedOut = r.error !== undefined && r.error.code === "ETIMEDOUT";
    if (timedOut) {
        console.error(`       TIMEOUT after ${TIMEOUT_MS}ms — the binary hung`);
        failed.push({ name, reason: "timeout" });
    } else if (r.status === null) {
        /*
         * Killed by a signal. Node reports no status at all for a process
         * that did not exit, and the runner used to print "exit null" —
         * which reads as a flake rather than as a crash. It was not a flake:
         * a stress test that segfaulted under a GC worker reported "exit
         * null" in one configuration and passed on the next run, and got
         * waved through as load. A killed binary is a crash and says so.
         */
        console.error(`       KILLED by ${r.signal ?? "a signal"} — no exit status`);
        failed.push({ name, reason: `killed by ${r.signal ?? "signal"}` });
    } else if (r.status !== 0) {
        failed.push({ name, reason: `exit ${r.status}` });
    }
}

console.log("");
if (failed.length > 0) {
    ui.log(`${bins.length - failed.length}/${bins.length} passed`);
    for (const f of failed) {
        console.error(`       FAIL ${f.name} (${f.reason})`);
    }
    process.exit(1);
}

ui.log(`all ${bins.length} passed`);
