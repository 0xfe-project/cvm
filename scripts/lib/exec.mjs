// scripts/lib/exec.mjs
//
// Subprocess helpers. Nothing here knows about C.

import { spawnSync } from "node:child_process";

/** Resolve a tool on PATH, or return null. */
export function which(tool) {
    const r = spawnSync("sh", ["-c", `command -v ${tool}`], { encoding: "utf8" });
    return r.status === 0 ? r.stdout.trim() : null;
}

/**
 * Run a command. Never throws: returns { status, stdout, stderr } so the
 * caller decides what a non-zero exit means.
 */
export function run(cmd, args, { cwd, verbose = false } = {}) {
    if (verbose) console.log(`        ${cmd} ${args.join(" ")}`);
    const r = spawnSync(cmd, args, { cwd, encoding: "utf8" });
    return {
        status: r.status ?? 1,
        stdout: r.stdout ?? "",
        stderr: r.stderr ?? "",
    };
}

/** Run and abort on failure, forwarding the tool's own diagnostics. */
export function runOrDie(cmd, args, { cwd, verbose = false, ui, what } = {}) {
    const r = run(cmd, args, { cwd, verbose });
    if (r.status !== 0) {
        if (r.stdout) process.stderr.write(r.stdout);
        if (r.stderr) process.stderr.write(r.stderr);
        if (ui) ui.fatal(`${what ?? cmd} failed (exit ${r.status})`);
        process.exit(1);
    }
    return r;
}
