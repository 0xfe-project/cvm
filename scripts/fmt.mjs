// scripts/fmt.mjs
//
// clang-format driver for cvm. Uses the project .clang-format.
//
//   node scripts/fmt.mjs           rewrite files in place
//   node scripts/fmt.mjs --check   report diffs, exit 1 if any (CI)
//
// The clang-format binary is located in this order:
//   1. $CLANG_FORMAT
//   2. `clang-format` on PATH
//   3. Xcode toolchain (ships with clang-format, but not clang-tidy)
//
// Version is pinned-ish: formatting output differs between major versions,
// so CI must use the same major as development. --check reports the version
// it used so mismatches are visible rather than mysterious.

import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const CHECK = process.argv.includes("--check");

const SOURCE_DIRS = ["rt", "vm", "tests"];
/* Assembly is not formatted: clang-format would reflow the
 * instruction columns and the .S conditionals. */
const EXTS = [".c", ".h"];
const SKIP_DIRS = new Set(["build", "refs", "node_modules", ".git"]);

function which(tool) {
    const r = spawnSync("sh", ["-c", `command -v ${tool}`], { encoding: "utf8" });
    return r.status === 0 ? r.stdout.trim() : null;
}

function findClangFormat() {
    if (process.env.CLANG_FORMAT) return process.env.CLANG_FORMAT;

    const onPath = which("clang-format");
    if (onPath) return onPath;

    const xcode =
        "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang-format";
    if (fs.existsSync(xcode)) return xcode;

    console.error("fmt: error: clang-format not found (set CLANG_FORMAT)");
    process.exit(1);
}

const FORMAT = findClangFormat();

function version() {
    const r = spawnSync(FORMAT, ["--version"], { encoding: "utf8" });
    return (r.stdout || r.stderr || "").trim();
}

function collect(dir) {
    const abs = path.join(ROOT, dir);
    if (!fs.existsSync(abs)) return [];
    const out = [];
    for (const ent of fs.readdirSync(abs, { withFileTypes: true })) {
        if (SKIP_DIRS.has(ent.name)) continue;
        const p = path.join(abs, ent.name);
        if (ent.isDirectory()) out.push(...collect(path.relative(ROOT, p)));
        else if (EXTS.some((e) => ent.name.endsWith(e))) out.push(p);
    }
    return out.sort();
}

const files = SOURCE_DIRS.flatMap(collect);

if (files.length === 0) {
    console.log("[fmt] no sources to format");
    process.exit(0);
}

console.log(`[fmt] ${version()}`);
console.log(`[fmt] ${CHECK ? "checking" : "formatting"} ${files.length} file(s)`);

const bad = [];
for (const f of files) {
    const rel = path.relative(ROOT, f);
    if (CHECK) {
        const r = spawnSync(FORMAT, ["--dry-run", "--Werror", f], { encoding: "utf8" });
        if (r.status !== 0) bad.push(rel);
    } else {
        const r = spawnSync(FORMAT, ["-i", f], { encoding: "utf8" });
        if (r.status !== 0) {
            console.error(`[fmt] failed: ${rel}`);
            process.stderr.write(r.stderr || "");
        }
    }
}

if (CHECK) {
    if (bad.length > 0) {
        console.error(`[fmt] ${bad.length} file(s) need formatting:`);
        for (const f of bad) console.error(`       ${f}`);
        console.error("[fmt] run: pnpm run fmt");
        process.exit(1);
    }
    console.log("[fmt] all files formatted correctly");
} else {
    console.log("[fmt] done");
}
