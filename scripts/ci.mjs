// scripts/ci.mjs
//
// The full verification matrix, in order, stopping at the first failure.
//
// Every build runs the freestanding audit, so "does vm/ still avoid libc"
// is checked in each configuration rather than once.
//
// The sanitizer configurations are not redundant with each other: asan
// catches out-of-bounds and use-after-free, tsan catches data races, and
// the concurrent block-list tests need tsan to mean anything.
//
//   node scripts/ci.mjs           run everything
//   node scripts/ci.mjs --quick   skip the sanitizer configurations

import { spawnSync } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { logger } from "./lib/util.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const ui = logger("ci");
const quick = process.argv.includes("--quick");

/** Each step is a name plus the commands it runs, in order. */
const FULL_MATRIX = [
    {
        name: "format check",
        cmds: [["scripts/fmt.mjs", "--check"]],
    },
    {
        name: "build + freestanding audit",
        cmds: [["scripts/build.mjs"]],
    },
    {
        name: "tests: debug",
        cmds: [
            ["scripts/build.mjs", "--test"],
            ["scripts/run-tests.mjs"],
        ],
    },
    {
        name: "tests: release",
        cmds: [
            ["scripts/build.mjs", "--test", "--release"],
            ["scripts/run-tests.mjs"],
        ],
    },
    {
        name: "tests: asan",
        cmds: [
            ["scripts/build.mjs", "--test", "--asan"],
            ["scripts/run-tests.mjs"],
        ],
    },
    {
        name: "tests: tsan",
        cmds: [
            ["scripts/build.mjs", "--test", "--tsan"],
            ["scripts/run-tests.mjs"],
        ],
    },
];

const matrix = quick
    ? FULL_MATRIX.filter((s) => !/asan|tsan/.test(s.name))
    : FULL_MATRIX;

const results = [];
let failed = null;

for (const step of matrix) {
    const started = Date.now();
    let ok = true;
    let detail = "";

    for (const [cmd, ...args] of step.cmds) {
        const r = spawnSync(process.execPath, [path.join(ROOT, cmd), ...args], {
            cwd: ROOT,
            encoding: "utf8",
        });

        if (r.status !== 0) {
            ok = false;
            detail = `${cmd} ${args.join(" ")} exited ${r.status}`;
            // Surface the tail of the failing output; the full log is long.
            const out = `${r.stdout ?? ""}${r.stderr ?? ""}`.trimEnd();
            const lines = out.split("\n");
            console.error(lines.slice(-25).join("\n"));
            break;
        }
    }

    const ms = Date.now() - started;
    results.push({ name: step.name, ok, ms, detail });
    ui.log(`${ok ? "ok  " : "FAIL"}  ${step.name} (${ms}ms)`);

    if (!ok) {
        failed = step.name;
        break;
    }
}

console.log("");
if (failed) {
    ui.log(`${results.length}/${matrix.length} steps ran; failed at: ${failed}`);
    process.exit(1);
}

const total = results.reduce((a, r) => a + r.ms, 0);
ui.log(`all ${results.length} steps passed in ${(total / 1000).toFixed(1)}s`);
